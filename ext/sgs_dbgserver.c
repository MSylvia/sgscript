

#include <sgs_int.h>

#include "sgs_dbgserver.h"


/*
	To successfully debug recursion issues,
	call stack limit must be raised
	to allow execution of debug code.
*/
#ifndef SGS_DBGSRV_STACK_EXTENSION
#define SGS_DBGSRV_STACK_EXTENSION SGS_MAX_CALL_STACK_SIZE
#endif

#define DBGSRV_BREAK_THREAD_CREATE  0x0001
#define DBGSRV_BREAK_THREAD_DESTROY 0x0002
#define DBGSRV_BREAK_THREAD_PAUSE   0x0004
#define DBGSRV_BREAK_THREAD_RESUME  0x0008
#define DBGSRV_BREAK_FUNCTION_ENTER 0x0010
#define DBGSRV_BREAK_FUNCTION_EXIT  0x0020
#define DBGSRV_BREAK_FUNCTION_STEP  0x0040
#define DBGSRV_BREAK_MESSAGE        0x0080
#define DBGSRV_BREAK_NEXT_T_CREATE  0x0100
#define DBGSRV_BREAK_NEXT_T_DESTROY 0x0200
#define DBGSRV_BREAK_NEXT_T_PAUSE   0x0400
#define DBGSRV_BREAK_NEXT_T_RESUME  0x0800
#define DBGSRV_BREAK_NEXT_F_ENTER   0x1000
#define DBGSRV_BREAK_NEXT_F_EXIT    0x2000
#define DBGSRV_BREAK_NEXT_F_STEP    0x4000
#define DBGSRV_BREAK_NEXT_MESSAGE   0x8000
#define DBGSRV_BREAK_NEXT_ANY       0xff00


typedef struct dbgPosInfo
{
	sgs_iFunc* func;
	int line;
}
dbgPosInfo;

struct sgs_DebugServer
{
	SGS_CTX;
	
	/* callbacks */
	sgs_MsgFunc pfn;
	void* pctx;
	sgs_HookFunc hfn;
	void* hctx;
	
	/* communication */
	int firstuse;
	int server_port;
	sgs_MemBuf input;
	
	/* state preservation */
	int inside;
	ptrdiff_t stkoff;
	ptrdiff_t stksize;
	
	/* introspection */
	int maxdepth;
	
	/* breakpoints */
	sgs_MemBuf breakpoints; /* array of dbgBreakpointInfo */
	unsigned brkflags;
	int brklev;
	dbgPosInfo linebreak_prev;
};

typedef struct dbgBreakpointInfo
{
	sgs_iStr* filename;
	sgs_iStr* funcname;
	int line;
}
dbgBreakpointInfo;

typedef struct dbgStateInfo
{
	SGS_CTX;
	int paused;
	int err_type;
	const char* err_message;
}
dbgStateInfo;

#define DBGSRV_MAX_CMD_NAME_LENGTH
#define DBGSRV_COMMANDS( def ) \
	def( continue ) \
	def( step ) \
	def( istep ) \
	def( quit ) \
	def( abort ) \
	def( breakpoint ) \
	def( ebreak ) \
	def( setcfg ) \
	def( instruction ) \
	def( where ) \
	def( error ) \
	def( dump ) \
	def( exec ) \
	def( print ) \
	def( include )
#define CID_LIST_ITEM( cmd ) DSC_ ## cmd,
#define STR_LIST_ITEM( cmd ) #cmd,

static const char* dbgsrv_command_names[] =
{
DBGSRV_COMMANDS( STR_LIST_ITEM )
};
#define DBGSRV_COMMAND_COUNT (sizeof(dbgsrv_command_names)/sizeof(dbgsrv_command_names[0]))
enum dbgsrv_command_values
{
DBGSRV_COMMANDS( CID_LIST_ITEM )
};

#define DBGSRV_ITERATE_BREAKPOINTS( D, bp ) \
	{ dbgBreakpointInfo* bp = SGS_ASSUME_ALIGNED( D->breakpoints.ptr, dbgBreakpointInfo ); \
		dbgBreakpointInfo* bpend = bp + D->breakpoints.size / sizeof(dbgBreakpointInfo); \
		for( ; bp != bpend; ++bp )
#define DBGSRV_ITERATE_END }

/*
	unused
static int dbgsrv_stricmpl( const char* str1, const char* str2low )
{
	while( *str1 && *str2low )
	{
		if( sgs_tolower( *str1 ) != *str2low )
			break;
		str1++;
		str2low++;
	}
	return sgs_tolower( *str1 ) - *str2low;
}

static int dbgsrv_getBool( const char* str )
{
	if( dbgsrv_stricmpl( str, "true" ) == 0 || dbgsrv_stricmpl( str, "on" ) == 0 ) return 1;
	if( dbgsrv_stricmpl( str, "false" ) == 0 || dbgsrv_stricmpl( str, "off" ) == 0 ) return 0;
	return atoi( str ) != 0;
}
*/

static dbgPosInfo dbgsrv_currentPos( SGS_CTX )
{
	sgs_StackFrame* sf = sgs_GetFramePtr( C, NULL, SGS_TRUE );
	if( sf && sf->iptr )
	{
		sgs_iFunc* F = sf->func->data.F;
		dbgPosInfo out = { F, sgs_func_lineinfo( F )[ sf->iptr - sgs_func_bytecode( F ) ] };
		return out;
	}
	else
	{
		/* use a non-NULL pointer to trigger line break even if there is no line info here */
		dbgPosInfo out = { (sgs_iFunc*) 1, 0 };
		return out;
	}
}

static void dbgsrv_runCode( sgs_DebugServer* D, dbgStateInfo* dsi, const char* code )
{
	sgs_ParserConfig prev_pcfg, dbg_pcfg;
	int i, rvc, compile_result;
	SGS_CTX = dsi->C;
	sgs_SizeVal ssz = sgs_StackSize( C );
	
	sgs_GetParserConfig( C, &prev_pcfg );
	dbg_pcfg = prev_pcfg;
	dbg_pcfg.ident_dollar_sign = 1;
	sgs_SetParserConfig( C, &dbg_pcfg );
	
	compile_result = sgs_PushSGSFunction( C, code );
	
	sgs_SetParserConfig( C, &prev_pcfg );
	
	if( SGS_FAILED( compile_result ) )
	{
		/* error already printed here */
		return;
	}
	rvc = sgs_XCall( C, 0 );
	
	for( i = 0; i < rvc; ++i )
	{
		sgs_DumpVar( C, sgs_StackItem( C, i - rvc ), D->maxdepth );
		{
			sgs_SizeVal bsz;
			char* buf = sgs_ToStringBuf( C, -1, &bsz );
			/* WP: string limit */
			sgs_Write( C, buf, (size_t) bsz );
			sgs_Write( C, "\n", 1 );
		}
		sgs_Pop( C, 1 );
	}
	sgs_SetStackSize( C, ssz );
}

static int dbgsrv_findCmd( sgs_DebugServer* D, const char** pcmd, const char** command_names,
	const char** parse_positions, int cmd_name_count, const char* cmd_prefix )
{
	int curcmd, off, endcount, match;
	const char* cmd = *pcmd;
	
	memcpy( parse_positions, command_names, sizeof(const char*) * (size_t) cmd_name_count );
	
	off = 0;
	endcount = cmd_name_count;
	while( *cmd == ' ' ) cmd++;
	/* iterate all commands, advance pointers if found a character match */
	while( sgs_isalpha( cmd[ off ] ) )
	{
		endcount = 0;
		for( curcmd = 0; curcmd < cmd_name_count; ++curcmd )
		{
			if( parse_positions[ curcmd ] - command_names[ curcmd ] == off &&
				*parse_positions[ curcmd ] == cmd[ off ] )
			{
				parse_positions[ curcmd ]++;
				match = curcmd;
				endcount++;
			}
		}
		off++;
	}
	*pcmd = &cmd[ off ];
	
	if( endcount == 0 )
	{
		if( cmd_prefix )
			sgs_ErrWritef( D->C, "Unknown subcommand of '%s'.\n", cmd_prefix );
		else
			sgs_ErrWritef( D->C, "Unknown command.\n" );
		return -1;
	}
	if( endcount > 1 )
	{
		if( cmd_prefix )
			sgs_ErrWritef( D->C, "Multiple matches for subcommand of '%s':\n", cmd_prefix );
		else
			sgs_ErrWritef( D->C, "Multiple matches for command:\n" );
		for( curcmd = 0; curcmd < cmd_name_count; ++curcmd )
		{
			if( parse_positions[ curcmd ] - command_names[ curcmd ] == off )
			{
				sgs_ErrWritef( D->C, "- %s\n", command_names[ curcmd ] );
			}
		}
		return -1;
	}
	return match;
}

static void dbgsrv_cfgBreak( sgs_DebugServer* D, const char** pcmd, int flagid )
{
	static const char* prefixes[] =
	{
		"ebreak thread create",
		"ebreak thread destroy",
		"ebreak thread pause",
		"ebreak thread resume",
		"ebreak function enter",
		"ebreak function exit",
		"ebreak function step",
		"ebreak message",
	};
	static const char* opts[] = { "off", "next", "always", "both" };
	unsigned baseflag = 1U << flagid;
	unsigned nextflag = baseflag << 8;
	unsigned mask = baseflag | nextflag;
	int which;
	const char* tmp[4];
	
	D->brkflags &= ~mask;
	switch( which = dbgsrv_findCmd( D, pcmd, opts, tmp, 4, prefixes[ flagid ] ) )
	{
	case 0: break;
	case 1: D->brkflags |= nextflag; break;
	case 2: D->brkflags |= baseflag; break;
	case 3: D->brkflags |= mask; break;
	}
	if( which >= 0 )
		sgs_ErrWritef( D->C, "break on event '%s' = %s\n", prefixes[ flagid ] + 7, opts[ which ] );
}

static void dbgsrv_printCurOp( sgs_DebugServer* D, dbgStateInfo* dsi )
{
	sgs_StackFrame* sf = sgs_GetFramePtr( dsi->C, NULL, SGS_TRUE );
	if( sf )
	{
		sgs_iFunc* F;
		if( sf->iptr )
		{
			F = sf->func->data.F;
			sgs_ErrWritef( D->C, "Current instruction:\n[%08X] ", *sf->iptr );
			sgsBC_DumpOpcode( D->C, sf->iptr, 1, sgs_func_bytecode( F ),
				sgs_func_consts( F ), sgs_func_lineinfo( F ) );
		}
		else if( sf->prev && sf->prev->iptr )
		{
			const sgs_instr_t* ip = sf->prev->iptr;
			if( ( sf->prev->flags & SGS_SF_ABORTED ) == 0 )
				ip--;
			F = sf->prev->func->data.F;
			sgs_ErrWritef( D->C, "Current instruction (parent frame):\n[%08X] ", *ip );
			sgsBC_DumpOpcode( D->C, ip, 1, sgs_func_bytecode( F ),
				sgs_func_consts( F ), sgs_func_lineinfo( F ) );
		}
		else
			sgs_ErrWritef( D->C, "Not running SGScript code.\n" );
	}
	else
		sgs_ErrWritef( D->C, "Not running code.\n" );
}

static void dbgsrv_stackRange( SGS_CTX, sgs_StackFrame* F, sgs_SizeVal* outfirst, sgs_SizeVal* outend )
{
	if( F && F->next )
	{
		*outfirst = F->next->stkoff / (sgs_SizeVal) sizeof( sgs_Variable );
		*outend = F->next->next
			? F->next->next->stkoff / (sgs_SizeVal) sizeof( sgs_Variable )
			: (sgs_SizeVal)( C->stack_top - C->stack_base );
	}
	else
	{
		*outfirst = (sgs_SizeVal)( C->stack_off - C->stack_base );
		*outend = (sgs_SizeVal)( C->stack_top - C->stack_base );
	}
}

static void dbgsrv_dumpVar( sgs_DebugServer* D, sgs_Variable* var, int ext )
{
	if( ext )
	{
	}
	else
	{
		sgsVM_VarDump( D->C, var );
		sgs_ErrWritef( D->C, "\n" );
	}
}

static void dbgsrv_dumpRegisters( sgs_DebugServer* D, SGS_CTX, sgs_StackFrame* F, int ext )
{
	sgs_SizeVal i, first, end;
	dbgsrv_stackRange( C, F, &first, &end );
	sgs_ErrWritef( D->C, "Registers (%s function) [%d]:%s",
		F ? ( F->iptr ? "SGScript" : "C" ) : "not in a", (int) ( end - first ),
		first >= end ? " none\n" : "\n" );
	for( i = first; i < end; ++i )
	{
		sgs_ErrWritef( D->C, " #%03d: ", ( i - first ) );
		dbgsrv_dumpVar( D, &C->stack_base[ i ], ext );
	}
	if( F && F->clsrref )
	{
		uint8_t* cl = (uint8_t*) F->clsrref->data;
		uint8_t ci, clsrcount = (uint8_t) *SGS_ASSUME_ALIGNED( cl + sizeof(sgs_Variable), sgs_clsrcount_t );
		sgs_ErrWritef( D->C, "Closures:%s", clsrcount ? "\n" : " none\n" );
		for( ci = 0; ci < clsrcount; ++ci )
		{
			sgs_ErrWritef( D->C, " #%03d: ", ci );
			dbgsrv_dumpVar( D, &F->clsrlist[ ci ]->var, ext );
		}
	}
	else sgs_ErrWritef( D->C, "Closures: none\n" );
}

static void dbgsrv_dumpVariables( sgs_DebugServer* D, SGS_CTX, sgs_StackFrame* F, int ext, int all )
{
	if( F == NULL )
	{
		sgs_ErrWritef( D->C, "Not in a function (no variables), dumping registers instead...\n" );
		dbgsrv_dumpRegisters( D, C, F, ext );
	}
	else if( F->iptr == NULL )
	{
		sgs_ErrWritef( D->C, "This is a C function (no variables), dumping registers instead...\n" );
		dbgsrv_dumpRegisters( D, C, F, ext );
	}
	else if( F->func->data.F->dbg_varinfo == NULL )
	{
		sgs_ErrWritef( D->C, "Function has no debug info (no variables), dumping registers instead...\n" );
		dbgsrv_dumpRegisters( D, C, F, ext );
	}
	else
	{
		char *varinfo, *varinfoend;
		uint8_t* cl;
		uint8_t clsrcount;
		uint32_t curinstrid, dbgvarsize;
		sgs_SizeVal first, end;
		dbgsrv_stackRange( C, F, &first, &end );
		if( F->clsrref )
		{
			cl = (uint8_t*) F->clsrref->data;
			clsrcount = (uint8_t) *SGS_ASSUME_ALIGNED( cl + sizeof(sgs_Variable), sgs_clsrcount_t );
		}
		curinstrid = F->iptr - sgs_func_bytecode( F->func->data.F );
		
		varinfo = F->func->data.F->dbg_varinfo;
		memcpy( &dbgvarsize, varinfo, sizeof(dbgvarsize) );
		varinfoend = varinfo + dbgvarsize;
		varinfo += sizeof(dbgvarsize);
		while( varinfo < varinfoend )
		{
			uint32_t from, to;
			int16_t pos;
			uint8_t len;
			
			memcpy( &from, varinfo, sizeof(from) );
			varinfo += sizeof(from);
			memcpy( &to, varinfo, sizeof(to) );
			varinfo += sizeof(to);
			memcpy( &pos, varinfo, sizeof(pos) );
			varinfo += sizeof(pos);
			memcpy( &len, varinfo, sizeof(len) );
			varinfo += sizeof(len);
			if( all || ( from <= curinstrid && curinstrid < to ) )
			{
				if( !all )
				{
					sgs_ErrWritef( D->C, from <= curinstrid && curinstrid < to ? ">" : " " );
				}
				
				if( pos == 0 ) sgs_ErrWritef( D->C, "[G] " );
				else if( pos < 0 ) sgs_ErrWritef( D->C, "[C%d] ", -1 - pos );
				else sgs_ErrWritef( D->C, "[L%d] ", pos - 1 );
				
				sgs_ErrWrite( D->C, varinfo, len );
				sgs_ErrWritef( D->C, ": " );
				
				if( pos == 0 )
				{
					sgs_Variable idx, val;
					sgs_InitStringBuf( C, &idx, varinfo, len );
					if( sgs_GetGlobal( C, idx, &val ) )
					{
						dbgsrv_dumpVar( D, &val, ext );
					}
					sgs_Release( C, &idx );
					sgs_Release( C, &val );
				}
				else if( pos < 0 )
				{
					pos = -1 - pos;
					if( pos >= clsrcount )
					{
						sgs_ErrWritef( D->C, "<ERROR:out of range>\n" );
					}
					else
					{
						dbgsrv_dumpVar( D, &F->clsrlist[ pos ]->var, ext );
					}
				}
				else
				{
					pos -= 1;
					if( pos >= end - first )
					{
						sgs_ErrWritef( D->C, "<ERROR:out of range>\n" );
					}
					else
					{
						dbgsrv_dumpVar( D, &C->stack_base[ first + pos ], ext );
					}
				}
			}
			varinfo += len;
		}
	}
}

static void dbgsrv_dumpBytecode( sgs_DebugServer* D, sgs_StackFrame* F )
{
	if( F && F->iptr )
	{
		sgsVM_DumpFunction( D->C, F->func->data.F, 0 );
	}
	else
	{
		sgs_ErrWritef( D->C, "No bytecode for C function.\n" );
	}
}

static void dbgsrv_execCmd( sgs_DebugServer* D, int cmd, const char* params, dbgStateInfo* dsi )
{
	D->inside = 1; /* some of these commands may produce messages */
	dsi->C->sf_count -= SGS_DBGSRV_STACK_EXTENSION;
	
	switch( cmd )
	{
	case DSC_continue:
		sgs_ErrWritef( D->C, "Continuing...\n" );
		dsi->paused = 0;
		break;
	case DSC_step:
		sgs_ErrWritef( D->C, "Stepping to next line...\n" );
		D->linebreak_prev = dbgsrv_currentPos( dsi->C );
		dsi->paused = 0;
		break;
	case DSC_istep:
		sgs_ErrWritef( D->C, "Stepping to next instruction...\n" );
		D->brkflags |= DBGSRV_BREAK_NEXT_F_STEP;
		dsi->paused = 0;
		break;
	case DSC_quit: {
			int code = atoi( params );
			sgs_ErrWritef( D->C, "Calling exit(%d)...\n", code );
			exit( code );
		} break;
	case DSC_abort:
		sgs_ErrWritef( D->C, "Aborting the thread...\n" );
		sgs_Abort( dsi->C );
		break;
	case DSC_breakpoint:
		/* TODO */
		break;
	case DSC_ebreak: {
			static const char* brkevcat[] = { "thread", "function", "message" };
			static const char* brkevthrtype[] = { "create", "destroy", "pause", "resume" };
			static const char* brkevfunctype[] = { "enter", "exit", "step" };
			const char* tmp[4];
			switch( dbgsrv_findCmd( D, &params, brkevcat, tmp, 3, "ebreak" ) )
			{
			case 0: /* thread */
				switch( dbgsrv_findCmd( D, &params, brkevthrtype, tmp, 4, "ebreak thread" ) )
				{
				case 0: dbgsrv_cfgBreak( D, &params, 0 ); break;
				case 1: dbgsrv_cfgBreak( D, &params, 1 ); break;
				case 2: dbgsrv_cfgBreak( D, &params, 2 ); break;
				case 3: dbgsrv_cfgBreak( D, &params, 3 ); break;
				}
				break;
			case 1: /* function */
				switch( dbgsrv_findCmd( D, &params, brkevfunctype, tmp, 3, "ebreak function" ) )
				{
				case 0: dbgsrv_cfgBreak( D, &params, 4 ); break;
				case 1: dbgsrv_cfgBreak( D, &params, 5 ); break;
				case 2: dbgsrv_cfgBreak( D, &params, 6 ); break;
				}
				break;
			case 2: /* message */
				dbgsrv_cfgBreak( D, &params, 7 ); break;
				break;
			}
		} break;
	case DSC_setcfg:
		while( *params == ' ' ) params++;
		if( strncmp( params, "dump_max_depth=", 15 ) == 0 )
		{
			D->maxdepth = atoi( params + 15 );
			sgs_ErrWritef( D->C, "dump_max_depth=%d\n", D->maxdepth );
		}
		else if( strncmp( params, "break_level=", 12 ) == 0 )
		{
			int level = 0;
			params += 12;
			
			if( strcmp( params, "info" ) ) level = SGS_INFO;
			else if( strcmp( params, "warning" ) ) level = SGS_WARNING;
			else if( strcmp( params, "error" ) ) level = SGS_ERROR;
			else if( strcmp( params, "apierr" ) ) level = SGS_APIERR;
			else if( strcmp( params, "interr" ) ) level = SGS_INTERR;
			else level = atoi( params );
			
			D->brklev = level;
			sgs_ErrWritef( D->C, "break_level=%d\n", level );
		}
		else
		{
			sgs_ErrWritef( D->C, "Unknown configuration option.\n" );
		}
		break;
	case DSC_instruction:
		dbgsrv_printCurOp( D, dsi );
		break;
	case DSC_where:
		sgs_WriteErrorInfo( dsi->C, SGS_ERRORINFO_STACK,
			(sgs_ErrorOutputFunc) sgs_ErrWritef, dsi->C, dsi->err_type, dsi->err_message );
		break;
	case DSC_error:
		sgs_WriteErrorInfo( dsi->C, SGS_ERRORINFO_ERROR,
			(sgs_ErrorOutputFunc) sgs_ErrWritef, dsi->C, dsi->err_type, dsi->err_message );
		break;
	case DSC_dump:
		while( *params == ' ' ) params++;
		{
			static const char* subcmds[] = {
				"stack", "globals", "objects", "frames",
				"breakpoints", "registers", "bytecode",
				"variables", "cvars",
			};
#define DSC_DUMP_SUBCMD_COUNT (sizeof(subcmds) / sizeof(subcmds[0]))
			const char* ppos[ DSC_DUMP_SUBCMD_COUNT ];
			switch( dbgsrv_findCmd( D, &params, subcmds, ppos, DSC_DUMP_SUBCMD_COUNT, "dump" ) )
			{
			case 0: sgs_Stat( dsi->C, SGS_STAT_DUMP_STACK ); break;
			case 1: sgs_Stat( dsi->C, SGS_STAT_DUMP_GLOBALS ); break;
			case 2: sgs_Stat( dsi->C, SGS_STAT_DUMP_OBJECTS ); break;
			case 3: sgs_Stat( dsi->C, SGS_STAT_DUMP_FRAMES ); break;
			case 4: {
					int i = 0;
					sgs_ErrWritef( D->C, "-- breakpoints --\n" );
					DBGSRV_ITERATE_BREAKPOINTS( D, bp )
					{
						i++;
						sgs_ErrWritef( D->C, "Breakpoint #%d:\n", i );
						sgs_ErrWritef( D->C, "  File: %s\n", bp->filename ? sgs_str_cstr( bp->filename ) : "<none>" );
						sgs_ErrWritef( D->C, "  Function: %s\n", bp->funcname ? sgs_str_cstr( bp->funcname ) : "<none>" );
						if( bp->line >= 0 )
							sgs_ErrWritef( D->C, "  Line: %d\n", bp->line );
						else
							sgs_ErrWritef( D->C, "  Line: <none>\n" );
					}
					DBGSRV_ITERATE_END;
				}
				break;
			case 5:
				dbgsrv_dumpRegisters( D, dsi->C, sgs_GetFramePtr( dsi->C, NULL, SGS_TRUE ), 0 );
				break;
			case 6:
				dbgsrv_dumpBytecode( D, sgs_GetFramePtr( dsi->C, NULL, SGS_TRUE ) );
				break;
			case 7:
				dbgsrv_dumpVariables( D, dsi->C, sgs_GetFramePtr( dsi->C, NULL, SGS_TRUE ), 0, 1 );
				break;
			case 8:
				dbgsrv_dumpVariables( D, dsi->C, sgs_GetFramePtr( dsi->C, NULL, SGS_TRUE ), 0, 0 );
				break;
			}
		}
		break;
	case DSC_exec:
		dbgsrv_runCode( D, dsi, params );
		break;
	case DSC_print:
		{
			sgs_MemBuf prepstr = sgs_membuf_create();
			sgs_membuf_appbuf( &prepstr, D->C, "return (", 8 );
			sgs_membuf_appbuf( &prepstr, D->C, params, strlen( params ) );
			sgs_membuf_appbuf( &prepstr, D->C, ");", 3 ); /* include \0 */
			
			dbgsrv_runCode( D, dsi, prepstr.ptr );
			
			sgs_membuf_destroy( &prepstr, D->C );
		}
		break;
	case DSC_include:
		while( *params == ' ' ) params++;
		sgs_ErrWritef( D->C, "Including '%s'...\n", params );
		sgs_Include( dsi->C, params );
		break;
	}
	
	dsi->C->sf_count += SGS_DBGSRV_STACK_EXTENSION;
	D->inside = 0;
}

static void dbgsrv_execInputStr( sgs_DebugServer* D, dbgStateInfo* dsi )
{
	size_t pos, eraseto = 0;
	const char* parse_positions[ DBGSRV_COMMAND_COUNT ];
	const char* cmd = D->input.ptr;
	for( pos = 0; pos < D->input.size; ++pos )
	{
		if( D->input.ptr[ pos ] == '\n' )
		{
			int match;
			D->input.ptr[ pos ] = '\0';
			
			match = dbgsrv_findCmd( D, &cmd, dbgsrv_command_names,
				parse_positions, DBGSRV_COMMAND_COUNT, NULL );
			
			if( match != -1 )
			{
				dbgsrv_execCmd( D, match, cmd, dsi );
			}
			
			eraseto = pos + 1;
			cmd = &D->input.ptr[ eraseto ];
		}
	}
	
	sgs_membuf_erase( &D->input, 0, eraseto );
}


static void dbgsrv_readStdin( sgs_DebugServer* D )
{
	/* read from STDIN */
#define STDIN_READ_BFR_SIZE 1024
	char bfr[ STDIN_READ_BFR_SIZE ];
	sgs_membuf_resize( &D->input, D->C, 0 );
	while( fgets( bfr, STDIN_READ_BFR_SIZE, stdin ) )
	{
		size_t len = strlen( bfr );
		sgs_membuf_appbuf( &D->input, D->C, bfr, len );
		if( len && bfr[ len - 1 ] == '\n' )
			break;
	}
	sgs_membuf_appchr( &D->input, D->C, 0 );
}

static void dbgsrv_interact( sgs_DebugServer* D, dbgStateInfo* dsi )
{
	SGS_CTX = dsi->C;
	if( D->firstuse )
	{
		sgs_ErrWritef( D->C, "----- Interactive SGScript Debug Inspector -----\n" );
		D->firstuse = 0;
	}
	D->stkoff = C->stack_off - C->stack_base;
	D->stksize = C->stack_top - C->stack_base;
	dsi->paused = 1;
	while( dsi->paused )
	{
		sgs_ErrWritef( D->C, "\n> " );
		dbgsrv_readStdin( D );
		if( ferror( stdin ) )
			break;
		dbgsrv_execInputStr( D, dsi );
	}
}

static void dbgsrv_printFunc( void* data, SGS_CTX, int type, const char* message )
{
	sgs_DebugServer* D = (sgs_DebugServer*) data;
	
	/* D->pfn( D->pctx, C, type, message ); */
	sgs_WriteErrorInfo( C, SGS_ERRORINFO_FULL, (sgs_ErrorOutputFunc) sgs_ErrWritef, C, type, message );
	if( D->inside || type < D->brklev )
		return;
	
	if( D->inside == 0 && type > D->brklev && SGS_HAS_FLAG( D->brkflags, DBGSRV_BREAK_MESSAGE ) )
	{
		dbgStateInfo dsi = { C, 1, type, message };
		dbgsrv_interact( D, &dsi );
	}
}

static void dbgsrv_hookFunc( void* data, SGS_CTX, int event_id )
{
	sgs_DebugServer* D = (sgs_DebugServer*) data;
	
	if( D->hfn )
		D->hfn( D->hctx, C, event_id );
	
	/* break on events */
	if( D->inside == 0 )
	{
		if( event_id == SGS_HOOK_ENTER )
		{
			if( D->brkflags & (DBGSRV_BREAK_FUNCTION_ENTER|DBGSRV_BREAK_NEXT_F_ENTER) )
			{
				dbgStateInfo dsi = { C, 1, 0, "Entered function" };
				D->brkflags &= ~(uint32_t)DBGSRV_BREAK_NEXT_F_ENTER;
				dbgsrv_interact( D, &dsi );
			}
		}
		else if( event_id == SGS_HOOK_EXIT )
		{
			if( D->brkflags & (DBGSRV_BREAK_FUNCTION_EXIT|DBGSRV_BREAK_NEXT_F_EXIT) )
			{
				dbgStateInfo dsi = { C, 1, 0, "Exited function" };
				D->brkflags &= ~(uint32_t)DBGSRV_BREAK_NEXT_F_EXIT;
				dbgsrv_interact( D, &dsi );
			}
		}
		else if( event_id == SGS_HOOK_STEP )
		{
			if( D->brkflags & (DBGSRV_BREAK_FUNCTION_STEP|DBGSRV_BREAK_NEXT_F_STEP) )
			{
				dbgStateInfo dsi = { C, 1, 0, "Reached next instruction" };
				D->brkflags &= ~(uint32_t)DBGSRV_BREAK_NEXT_F_STEP;
				dbgsrv_printCurOp( D, &dsi );
				dbgsrv_interact( D, &dsi );
			}
			else if( D->linebreak_prev.func )
			{
				dbgPosInfo cp = dbgsrv_currentPos( C );
				if( D->linebreak_prev.func != cp.func || D->linebreak_prev.line != cp.line )
				{
					dbgStateInfo dsi = { C, 1, 0, "Reached next line" };
					D->linebreak_prev.func = NULL;
					dbgsrv_printCurOp( D, &dsi ); /* TODO print source line */
					dbgsrv_interact( D, &dsi );
				}
			}
		}
	}
}


sgs_DebugServer* sgs_CreateDebugServer( SGS_CTX, int port )
{
	sgs_DebugServer* D = sgs_Alloc( sgs_DebugServer );
	
	D->C = sgs_RootContext( C );
	
	sgs_GetMsgFunc( C, &D->pfn, &D->pctx );
	sgs_SetMsgFunc( C, dbgsrv_printFunc, D );
	sgs_GetHookFunc( C, &D->hfn, &D->hctx );
	sgs_SetHookFunc( C, dbgsrv_hookFunc, D );
	
	D->firstuse = 1;
	D->server_port = port;
	D->input = sgs_membuf_create();
	
	D->inside = 0;
	D->stkoff = 0;
	D->stksize = 0;
	
	D->maxdepth = 5;
	
	D->breakpoints = sgs_membuf_create();
	D->brkflags = DBGSRV_BREAK_MESSAGE;
	D->brklev = SGS_WARNING;
	D->linebreak_prev.func = NULL;
	D->linebreak_prev.line = 0;
	
	return D;
}

void sgs_CloseDebugServer( sgs_DebugServer* D )
{
	if( ( D->brkflags & DBGSRV_BREAK_NEXT_ANY ) || D->linebreak_prev.func )
	{
		sgs_ErrWritef( D->C, "Scripting engine was stopped before next breakpoint could be reached.\n" );
	}
	sgs_SetMsgFunc( D->C, D->pfn, D->pctx );
	sgs_SetHookFunc( D->C, D->hfn, D->hctx );
	sgs_membuf_destroy( &D->input, D->C );
	sgs_membuf_destroy( &D->breakpoints, D->C );
	sgs_Free( D->C, D );
}

void sgs_DebugServerCmd( sgs_DebugServer* D, const char* cmd )
{
	dbgStateInfo dsi = { D->C, 0, 0, "No error." };
	if( cmd == NULL )
	{
		dbgsrv_interact( D, &dsi );
	}
	else
	{
		sgs_membuf_setstr( &D->input, D->C, cmd );
		dbgsrv_execInputStr( D, &dsi );
	}
}

