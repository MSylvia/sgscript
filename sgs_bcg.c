

#include "sgs_bcg.h"
#include "sgs_ctx.h"
#include "sgs_proc.h"


/* register allocation */
static int16_t __ra_alloc( const uint32_t* regs, uint32_t size )
{
	uint32_t i, j;
	for( i = 0; i < size; ++i )
	{
		if( regs[ i ] == 0 )
			return (int16_t)( i * 32 );
		else if( regs[ i ] != 0xffffffff )
		{
			for( j = 0; j < 32; ++j )
				if( ( regs[ i ] & ( 1 << j ) ) == 0 )
					return (int16_t)( i * 32 + j );
		}
	}
	return -1;
}
static void __ra_free( uint32_t* regs, uint32_t size, int16_t off )
{
	uint32_t i = ((uint32_t)off) / 32;
	uint32_t j = ((uint32_t)off) % 32;
	UNUSED( size );
	sgs_BreakIf( i >= size );
	regs[ i ] &= ~( 1 << j );
}
#define RA_DECL( name ) uint32_t name [ 8 ];
#define RA_CTOR( name ) { name [0]=0; name [1]=0; name [2]=0; name [3]=0; name [4]=0; name [5]=0; name [6]=0; name[7]=0; }
#define RA_DTOR( name )
#define RA_ALLOC( name ) __ra_alloc( name, 8 )
#define RA_FREE( name, pos ) __ra_free( name, 8, pos )


static sgs_CompFunc* make_compfunc()
{
	sgs_CompFunc* func = sgs_Alloc( sgs_CompFunc );
	func->consts = membuf_create();
	func->code = membuf_create();
	return func;
}


static void fctx_binfo_add( sgs_FuncCtx* fctx, uint32_t ioff, uint16_t loop, uint8_t iscont )
{
	sgs_BreakInfo* binfo = sgs_Alloc( sgs_BreakInfo );
	binfo->jdoff = ioff;
	binfo->numlp = loop;
	binfo->iscont = iscont;
	binfo->next = fctx->binfo;
	fctx->binfo = binfo;
}

static void fctx_binfo_rem( sgs_FuncCtx* fctx, sgs_BreakInfo* prev )
{
	sgs_BreakInfo* pn;
	if( prev )
	{
		pn = prev->next;
		prev->next = prev->next->next;
		sgs_Free( pn );
	}
	else
	{
		pn = fctx->binfo;
		fctx->binfo = fctx->binfo->next;
		sgs_Free( pn );
	}
}

static sgs_FuncCtx* fctx_create()
{
	sgs_FuncCtx* fctx = sgs_Alloc( sgs_FuncCtx );
	fctx->func = TRUE;
	fctx->regs = 0;
	fctx->vars = strbuf_create();
	fctx->gvars = strbuf_create();
	fctx->loops = 0;
	fctx->binfo = NULL;
	return fctx;
}

static void fctx_destroy( sgs_FuncCtx* fctx )
{
	while( fctx->binfo )
		fctx_binfo_rem( fctx, NULL );
	strbuf_destroy( &fctx->vars );
	strbuf_destroy( &fctx->gvars );
	sgs_Free( fctx );
}

static void fctx_dump( sgs_FuncCtx* fctx )
{
	printf( "Type: %s\nGlobals: %s\nVariables: %s\n", fctx->func ? "Function" : "Main code", fctx->gvars.ptr, fctx->vars.ptr );
}


static void dump_opcode( char* ptr, int32_t size )
{
	char* pend = ptr + size;
	while( ptr < pend )
	{
		printf( "\t|  " );
		switch( *ptr++ )
		{
#define STOC( wat ) case SI_##wat: printf( #wat "   " ); break
		STOC( NOP );

		case SI_PUSHC: printf( "PUSH_C %d", (int) AS_UINT16( ptr ) ); ptr += 2; break;
		case SI_PUSHS: printf( "PUSH_STK %d", (int) AS_INT16( ptr ) ); ptr += 2; break;
		case SI_PUSHN: printf( "PUSH_NULL" ); break;
		STOC( DUP );

		STOC( COPY );
		STOC( POP );
		case SI_POPN: printf( "POP_N %d", (int) AS_UINT8( ptr++ ) ); break;
		case SI_POPS: printf( "POP_STK %d", (int) AS_INT16( ptr ) ); ptr += 2; break;
		case SI_RETN: printf( "RETURN %d", (int) AS_UINT8( ptr++ ) ); break;
		case SI_JUMP: printf( "JUMP %d", (int) AS_INT16( ptr ) ); ptr += 2; break;
		case SI_JMPF: printf( "JMP_F %d", (int) AS_INT16( ptr ) ); ptr += 2; break;
		case SI_CALL: printf( "CALL %d %d", (int) AS_UINT8( ptr ), (int) AS_UINT8( ptr + 1 ) ); ptr += 2; break;
		case SI_GETVAR: printf( "GET_VAR" ); break;
		case SI_SETVAR: printf( "SET_VAR" ); break;
		case SI_GETPROP: printf( "GET_PROP" ); break;
		case SI_SETPROP: printf( "SET_PROP" ); break;
		case SI_GETINDEX: printf( "GET_IDX" ); break;
		case SI_SETINDEX: printf( "SET_IDX" ); break;

		STOC( CONCAT );
		STOC( BOOL_AND );
		STOC( BOOL_OR );
		STOC( NEGATE );
		STOC( BOOL_INV );
		STOC( INVERT );

		STOC( INC );
		STOC( DEC );
		STOC( ADD );
		STOC( SUB );
		STOC( MUL );
		STOC( DIV );
		STOC( MOD );

		STOC( AND );
		STOC( OR );
		STOC( XOR );
		STOC( LSH );
		STOC( RSH );

		STOC( SEQ );
		STOC( SNEQ );
		STOC( EQ );
		STOC( NEQ );
		STOC( LT );
		STOC( LTE );
		STOC( GT );
		STOC( GTE );
#undef STOC

		default: printf( "<error>" ); break;
		}
		printf( "\t|\n" );
	}
}


static int find_var( StrBuf* S, char* str, int len )
{
	char* ptr = S->ptr;
	char* pend = ptr + S->size;
	const char* cstr = str;
	int difs = 0, at = 0, left = len;

	while( ptr < pend )
	{
		if( *ptr == '=' )
		{
			if( difs == 0 && !left )
				return at;
			difs = 0;
			cstr = str;
			left = len;
			ptr++;
			at++;
		}
		else
		{
			difs += abs( *cstr - *ptr );
			ptr += *ptr != '=';
			cstr += ( left -= 1 ) > 0;
		}
	}
	return -1;
}

static int add_var( StrBuf* S, char* str, int len )
{
	int pos = find_var( S, str, len );
	if( pos < 0 )
	{
		strbuf_appbuf( S, str, len );
		strbuf_appchr( S, '=' );
		return TRUE;
	}
	return FALSE;
}

/* simplifies writing code */
#define BYTE( c )		membuf_appchr( &func->code, (c) )
#define DATA( ptr, sz )	membuf_appbuf( &func->code, (ptr), (sz) )


static void preparse_varlist( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	node = node->child;
	while( node )
	{
		if( add_var( &C->fctx->vars, (char*) node->token + 2, node->token[ 1 ] ) )
			BYTE( SI_PUSHN );
		node = node->next;
	}
}

static void preparse_gvlist( SGS_CTX, FTNode* node )
{
	node = node->child;
	while( node )
	{
		add_var( &C->fctx->gvars, (char*) node->token + 2, node->token[ 1 ] );
		node = node->next;
	}
}

static void preparse_varlists( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	if( node->type == SFT_VARLIST )
		preparse_varlist( C, func, node );
	else if( node->type == SFT_GVLIST )
		preparse_gvlist( C, node );
	else if( node->type == SFT_OPER )
	{
		if( ST_OP_ASSIGN( *node->token ) && node->child && node->child->type == SFT_IDENT )
		{
			if( add_var( &C->fctx->vars, (char*) node->child->token + 2, node->child->token[ 1 ] ) )
				BYTE( SI_PUSHN );
		}
	}
	else if( node->child && node->type != SFT_FUNC )
		preparse_varlists( C, func, node->child );
	if( node->next )
		preparse_varlists( C, func, node->next );
}

static int preparse_arglist( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	UNUSED( func );
	node = node->child;
	while( node )
	{
		if( !add_var( &C->fctx->vars, (char*) node->token + 2, node->token[ 1 ] ) )
		{
			sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), "Cannot redeclare arguments with the same name." );
			return 0;
		}
		node = node->next;
	}
	return 1;
}


static int add_const_b( SGS_CTX, sgs_CompFunc* func, int32_t bval )
{
	sgs_Variable** vbeg = (sgs_Variable**) func->consts.ptr;
	sgs_Variable** vend = (sgs_Variable**) ( func->consts.ptr + func->consts.size );
	sgs_Variable** var = vbeg;
	sgs_Variable* nvar;
	while( var < vend )
	{
		if( (*var)->type == SVT_BOOL && (*var)->data.B == bval )
			return var - vbeg;
		var++;
	}
	UNUSED( C );

	nvar = sgsVM_VarCreate( C, SVT_BOOL );
	nvar->data.B = bval;
	membuf_appbuf( &func->consts, &nvar, sizeof( var ) );
	return vend - vbeg;
}

static int add_const_i( SGS_CTX, sgs_CompFunc* func, sgs_Integer ival )
{
	sgs_Variable** vbeg = (sgs_Variable**) func->consts.ptr;
	sgs_Variable** vend = (sgs_Variable**) ( func->consts.ptr + func->consts.size );
	sgs_Variable** var = vbeg;
	sgs_Variable* nvar;
	while( var < vend )
	{
		if( (*var)->type == SVT_INT && (*var)->data.I == ival )
			return var - vbeg;
		var++;
	}
	UNUSED( C );

	nvar = sgsVM_VarCreate( C, SVT_INT );
	nvar->data.I = ival;
	membuf_appbuf( &func->consts, &nvar, sizeof( var ) );
	return vend - vbeg;
}

static int add_const_r( SGS_CTX, sgs_CompFunc* func, sgs_Real rval )
{
	sgs_Variable** vbeg = (sgs_Variable**) func->consts.ptr;
	sgs_Variable** vend = (sgs_Variable**) ( func->consts.ptr + func->consts.size );
	sgs_Variable** var = vbeg;
	sgs_Variable* nvar;
	while( var < vend )
	{
		if( (*var)->type == SVT_REAL && (*var)->data.R == rval )
			return var - vbeg;
		var++;
	}
	UNUSED( C );

	nvar = sgsVM_VarCreate( C, SVT_REAL );
	nvar->data.R = rval;
	membuf_appbuf( &func->consts, &nvar, sizeof( var ) );
	return vend - vbeg;
}

static int add_const_s( SGS_CTX, sgs_CompFunc* func, int32_t len, const char* str )
{
	sgs_Variable** vbeg = (sgs_Variable**) func->consts.ptr;
	sgs_Variable** vend = (sgs_Variable**) ( func->consts.ptr + func->consts.size );
	sgs_Variable** var = vbeg;
	sgs_Variable* nvar;
	while( var < vend )
	{
		if( (*var)->type == SVT_STRING
			&& (*var)->data.S.size == len
			&& memcmp( (*var)->data.S.ptr, str, len ) == 0 )
			return var - vbeg;
		var++;
	}
	UNUSED( C );

	nvar = sgsVM_VarCreateString( C, str, len );
	membuf_appbuf( &func->consts, &nvar, sizeof( var ) );
	return vend - vbeg;
}

static int add_const_f( SGS_CTX, sgs_CompFunc* func, sgs_CompFunc* nf )
{
	sgs_Variable* var = sgsVM_VarCreate( C, SVT_FUNC );
	char* fdata = sgs_Alloc_n( char, nf->consts.size + nf->code.size );
	int pos;
	UNUSED( C );

	memcpy( fdata, nf->consts.ptr, nf->consts.size );
	memcpy( fdata + nf->consts.size, nf->code.ptr, nf->code.size );

	var->data.F.bytecode = fdata;
	var->data.F.instr_off = nf->consts.size;
	var->data.F.size = nf->consts.size + nf->code.size;

	membuf_destroy( &nf->consts );
	membuf_destroy( &nf->code );
	sgs_Free( nf );

	pos = func->consts.size / sizeof( var );
	membuf_appbuf( &func->consts, &var, sizeof( var ) );
	return pos;
}

#define INTERNAL_ERROR( loff ) sgs_printf( C, SGS_ERROR, -1, "INTERNAL ERROR occured in file %s [%d]", __FILE__, __LINE__ - (loff) )

static int op_pick_opcode( int oper, int binary )
{
	if( !binary )
	{
		if( oper == ST_OP_ADD ) return 0;
		if( oper == ST_OP_SUB )	return SI_NEGATE;
		if( oper == ST_OP_NOT ) return SI_BOOL_INV;
		if( oper == ST_OP_INV ) return SI_INVERT;
	}

	switch( oper )
	{
	case ST_OP_ADD: case ST_OP_ADDEQ: return SI_ADD;
	case ST_OP_SUB: case ST_OP_SUBEQ: return SI_SUB;
	case ST_OP_MUL: case ST_OP_MULEQ: return SI_MUL;
	case ST_OP_DIV: case ST_OP_DIVEQ: return SI_DIV;
	case ST_OP_MOD: case ST_OP_MODEQ: return SI_MOD;

	case ST_OP_AND: case ST_OP_ANDEQ: return SI_AND;
	case ST_OP_OR: case ST_OP_OREQ: return SI_OR;
	case ST_OP_XOR: case ST_OP_XOREQ: return SI_XOR;
	case ST_OP_LSH: case ST_OP_LSHEQ: return SI_LSH;
	case ST_OP_RSH: case ST_OP_RSHEQ: return SI_RSH;

	case ST_OP_CAT: case ST_OP_CATEQ: return SI_CONCAT;
	case ST_OP_BLAND: case ST_OP_BLAEQ: return SI_BOOL_AND;
	case ST_OP_BLOR: case ST_OP_BLOEQ: return SI_BOOL_OR;

	case ST_OP_SEQ: return SI_SEQ;
	case ST_OP_SNEQ: return SI_SNEQ;
	case ST_OP_EQ: return SI_EQ;
	case ST_OP_NEQ: return SI_NEQ;
	case ST_OP_LESS: return SI_LT;
	case ST_OP_LEQ: return SI_LTE;
	case ST_OP_GRTR: return SI_GT;
	case ST_OP_GEQ: return SI_GTE;

	default: return 0;
	}
}


static int compile_node( SGS_CTX, sgs_CompFunc* func, FTNode* node );
static int compile_node_r( SGS_CTX, sgs_CompFunc* func, FTNode* node );
static int compile_node_w( SGS_CTX, sgs_CompFunc* func, FTNode* node );
static int compile_oper( SGS_CTX, sgs_CompFunc* func, FTNode* node, int expect, int out );


static void compile_ident( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	int16_t pos = add_const_s( C, func, node->token[ 1 ], (const char*) node->token + 2 );
	BYTE( SI_PUSHC );
	DATA( &pos, 2 );
}

static int is_keyword( TokenList tok, const char* text )
{
	return *tok == ST_KEYWORD && tok[ 1 ] == strlen( text ) && strncmp( (const char*) tok + 2, text, tok[ 1 ] ) == 0;
}

static int compile_ident_r( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	int16_t pos;
	if( is_keyword( node->token, "null" ) ) { BYTE( SI_PUSHN ); return 1; }
	if( is_keyword( node->token, "true" ) )
	{
		pos = add_const_b( C, func, TRUE );
		BYTE( SI_PUSHC );
		DATA( &pos, 2 );
		return 1;
	}
	if( is_keyword( node->token, "false" ) )
	{
		pos = add_const_b( C, func, FALSE );
		BYTE( SI_PUSHC );
		DATA( &pos, 2 );
		return 1;
	}
	if( *node->token == ST_KEYWORD )
	{
		sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), "Cannot read from this keyword" );
		return 0;
	}

	if( C->fctx->func )
	{
		int16_t gpos = find_var( &C->fctx->gvars, (char*) node->token + 2, node->token[ 1 ] );
		if( gpos >= 0 )
			pos = -1;
		else
		{
			pos = find_var( &C->fctx->vars, (char*) node->token + 2, node->token[ 1 ] );
			if( pos < 0 )
				pos = -1; /* read from globals by default */
		}
	}
	else
	{
		pos = find_var( &C->fctx->vars, (char*) node->token + 2, node->token[ 1 ] );
	}

	if( pos >= 0 )
	{
		BYTE( SI_PUSHS );
		DATA( &pos, 2 );
	}
	else
	{
		compile_ident( C, func, node );
		BYTE( SI_GETVAR );
	}
	return 1;
}

static int compile_ident_w( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	int16_t pos;
	if( *node->token == ST_KEYWORD )
	{
		sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), "Cannot write to reserved keywords" );
		return 0;
	}

	if( C->fctx->func )
	{
		int16_t gpos = find_var( &C->fctx->gvars, (char*) node->token + 2, node->token[ 1 ] );
		if( gpos >= 0 )
			pos = -1;
		else
		{
			add_var( &C->fctx->vars, (char*) node->token + 2, node->token[ 1 ] );
			pos = find_var( &C->fctx->vars, (char*) node->token + 2, node->token[ 1 ] );
		}
	}
	else
	{
		pos = find_var( &C->fctx->vars, (char*) node->token + 2, node->token[ 1 ] );
	}

	if( pos >= 0 )
	{
		BYTE( SI_POPS );
		DATA( &pos, 2 );
	}
	else
	{
		compile_ident( C, func, node );
		BYTE( SI_SETVAR );
	}
	return 1;
}


static int compile_const( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	int16_t pos;
	if( *node->token == ST_NUMINT )
	{
		pos = add_const_i( C, func, AS_INTEGER( node->token + 1 ) );
	}
	else if( *node->token == ST_NUMREAL )
	{
		pos = add_const_r( C, func, AS_REAL( node->token + 1 ) );
	}
	else if( *node->token == ST_STRING )
	{
		pos = add_const_s( C, func, AS_INT32( node->token + 1 ), (const char*) node->token + 5 );
	}
	else
	{
		sgs_Printf( C, SGS_ERROR, -1, "INTERNAL ERROR: constant doesn't have a token of type int/real/string attached" );
		return 0;
	}
	BYTE( SI_PUSHC );
	DATA( &pos, 2 );
	return 1;
}


static int compile_fcall( SGS_CTX, sgs_CompFunc* func, FTNode* node, int expect )
{
	int i = 0;

	FUNC_ENTER;
	if( !compile_node_r( C, func, node->child ) ) return 0;
	if( node->child->type == SFT_OPER && *node->child->token == ST_OP_MMBR )
	{
		FUNC_ENTER;
		if( !compile_node_r( C, func, node->child->child ) ) return 0;
		i++;
	}
	FUNC_ENTER;
	if( !compile_node_r( C, func, node->child->next ) ) return 0;
	{
		FTNode* n = node->child->next->child;
		while( n )
		{
			i++;
			n = n->next;
		}
	}

	BYTE( SI_CALL );
	BYTE( i );
	BYTE( expect );
	return 1;
}

static int compile_index( SGS_CTX, sgs_CompFunc* func, FTNode* node, int out )
{
	FUNC_ENTER;
	if( !compile_node_r( C, func, node->child ) ) return 0;
	FUNC_ENTER;
	if( !compile_node_r( C, func, node->child->next ) ) return 0;
	BYTE( out ? SI_SETINDEX : SI_GETINDEX );
	return 1;
}

static int compile_oper( SGS_CTX, sgs_CompFunc* func, FTNode* node, int expect, int out )
{
	int i = ST_OP_ASSIGN( *node->token );
	FUNC_BEGIN;

	/* Increment / decrement */
	if( *node->token == ST_OP_INC || *node->token == ST_OP_DEC )
	{
		FUNC_ENTER;
		if( !compile_node_r( C, func, node->child ) ) goto fail;
		if( node->type != SFT_OPER_P ) BYTE( *node->token == ST_OP_INC ? SI_INC : SI_DEC );
		if( expect )
		{
			if( expect != 1 )
			{
				sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), "Too many expected outputs for operator." );
				goto fail;
			}
			BYTE( SI_DUP );
		}
		if( node->type == SFT_OPER_P ) BYTE( *node->token == ST_OP_INC ? SI_INC : SI_DEC );
		if( !compile_node_w( C, func, node->child ) ) goto fail;
	}
	/* Assignment */
	else if( i )
	{
		char op;
		if( *node->token != ST_OP_SET && *node->token != ST_OP_COPY )
		{
			FUNC_ENTER;
			if( !compile_node_r( C, func, node->child ) ) goto fail;
		}
		FUNC_ENTER;
		if( !compile_node_r( C, func, node->child->next ) ) goto fail;

		op = op_pick_opcode( *node->token, 1 );
		if( op ) BYTE( op );

		if( *node->token != ST_OP_COPY )
		{
			FUNC_ENTER;
			if( !compile_node_w( C, func, node->child ) ) goto fail;
		}
		else
		{
			FUNC_ENTER;
			if( !compile_node_r( C, func, node->child ) ) goto fail;
			BYTE( SI_COPY );
		}

		if( expect )
		{
			FUNC_ENTER;
			if( !compile_node_r( C, func, node->child ) ) goto fail;
		}
	}
	/* Any other, needs expected output */
	else if( expect )
	{
		if( expect != 1 && expect != 2 )
		{
			sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), "Too many expected outputs for operator." );
			goto fail;
		}

		if( *node->token == ST_OP_MMBR )
		{
			FUNC_ENTER;
			if( !compile_node_r( C, func, node->child ) ) goto fail;
			if( expect == 2 )
			{
				BYTE( SI_DUP );
			}

			if( node->child->next->type == SFT_IDENT )
				compile_ident( C, func, node->child->next );
			else
			{
				FUNC_ENTER;
				if( !compile_node_r( C, func, node->child->next ) ) goto fail;
			}
			BYTE( out ? SI_SETPROP : SI_GETPROP );
		}
		else
		{
			char op;
			FUNC_ENTER;
			if( !compile_node_r( C, func, node->child ) ) goto fail;
			if( expect == 2 )
			{
				BYTE( SI_DUP );
			}
			if( node->child->next )
			{
				FUNC_ENTER;
				if( !compile_node_r( C, func, node->child->next ) ) goto fail;
			}

			op = op_pick_opcode( *node->token, !!node->child->next );
			if( op ) BYTE( op );
		}
	}

	FUNC_END;
	return 1;

fail:
	C->state |= SGS_HAS_ERRORS;
	FUNC_END;
	return 0;
}


static int compile_breaks( SGS_CTX, sgs_CompFunc* func, uint8_t iscont )
{
	sgs_BreakInfo* binfo = C->fctx->binfo, *prev = NULL;
	while( binfo )
	{
		if( binfo->numlp == C->fctx->loops && binfo->iscont == iscont )
		{
			*(int16_t*)( func->code.ptr + binfo->jdoff ) = func->code.size - 2 - binfo->jdoff;
			binfo = binfo->next;
			fctx_binfo_rem( C->fctx, prev );
		}
		else
		{
			prev = binfo;
			binfo = binfo->next;
		}
	}
	return 1;
}


static int compile_node_w( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	FUNC_BEGIN;
	switch( node->type )
	{
	case SFT_IDENT:
	case SFT_KEYWORD:
		FUNC_HIT( "W_IDENT" );
		if( !compile_ident_w( C, func, node ) ) goto fail;
		break;

	case SFT_CONST:
		FUNC_HIT( "W_CONST" );
		sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), "Cannot write to constants." );
		goto fail;

	case SFT_OPER:
	case SFT_OPER_P:
		FUNC_HIT( "W_OPER" );
		if( !compile_oper( C, func, node, 1, 1 ) ) goto fail;
		break;

	case SFT_FCALL:
		FUNC_HIT( "W_FCALL" );
		if( !compile_fcall( C, func, node, 1 ) ) goto fail;
		break;

	case SFT_INDEX:
		FUNC_HIT( "W_INDEX" );
		if( !compile_index( C, func, node, 1 ) ) goto fail;
		break;

	default:
		sgs_Printf( C, SGS_ERROR, -1, "Unexpected tree node [uncaught/internal error]." );
		goto fail;
	}
	FUNC_END;
	return 1;

fail:
	FUNC_END;
	return 0;
}
static int compile_node_r( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	FUNC_BEGIN;
	switch( node->type )
	{
	case SFT_IDENT:
	case SFT_KEYWORD:
		FUNC_HIT( "R_IDENT" );
		if( !compile_ident_r( C, func, node ) ) goto fail;
		break;

	case SFT_CONST:
		FUNC_HIT( "R_CONST" );
		if( !compile_const( C, func, node ) ) goto fail;
		break;

	case SFT_OPER:
	case SFT_OPER_P:
		FUNC_HIT( "R_OPER" );
		if( !compile_oper( C, func, node, 1, 0 ) ) goto fail;
		break;

	case SFT_FCALL:
		FUNC_HIT( "R_FCALL" );
		if( !compile_fcall( C, func, node, 1 ) ) goto fail;
		break;

	case SFT_INDEX:
		FUNC_HIT( "R_INDEX" );
		if( !compile_index( C, func, node, 0 ) ) goto fail;
		break;

	case SFT_EXPLIST:
		FUNC_HIT( "R_EXPLIST" );
		{
			FTNode* n = node->child;
			while( n )
			{
				FUNC_ENTER;
				if( !compile_node_r( C, func, n ) )
					goto fail;
				n = n->next;
			}
		}
		break;

	default:
		sgs_Printf( C, SGS_ERROR, -1, "Unexpected tree node [uncaught/internal error]." );
		goto fail;
	}
	FUNC_END;
	return 1;

fail:
	FUNC_END;
	return 0;
}

static int compile_node( SGS_CTX, sgs_CompFunc* func, FTNode* node )
{
	int32_t i = 0;
	FUNC_BEGIN;

	switch( node->type )
	{
	/* ignore these items if they're leading in statements */
	case SFT_IDENT: break;
	case SFT_KEYWORD: break;
	case SFT_CONST: break;

	case SFT_OPER:
	case SFT_OPER_P:
		FUNC_HIT( "OPERATOR" );
		if( !compile_oper( C, func, node, 0, 0 ) ) goto fail;
		break;

	case SFT_FCALL:
		FUNC_HIT( "FCALL" );
		if( !compile_fcall( C, func, node, 0 ) ) goto fail;
		break;

	case SFT_EXPLIST:
		FUNC_HIT( "EXPLIST" );
		{
			FTNode* n = node->child;
			while( n )
			{
				FUNC_ENTER;
				if( !compile_node( C, func, n ) )
					goto fail;
				n = n->next;
			}
		}
		break;

	case SFT_RETURN:
		FUNC_HIT( "RETURN" );
		{
			int num = 0;
			FTNode* n = node->child;
			while( n )
			{
				FUNC_ENTER;
				if( !compile_node_r( C, func, n ) ) goto fail;
				n = n->next;
				num++;
			}
			BYTE( SI_RETN );
			BYTE( num );
		}
		break;

	case SFT_BLOCK:
		FUNC_HIT( "BLOCK" );
		node = node->child;
		while( node )
		{
			FUNC_ENTER;
			if( !compile_node( C, func, node ) ) goto fail;
			node = node->next;
		}
		break;

	case SFT_IFELSE:
		FUNC_HIT( "IF/ELSE" );
		FUNC_ENTER;
		if( !compile_node_r( C, func, node->child ) ) goto fail;
		BYTE( SI_JMPF );
		{
			int32_t jp1, jp2 = 0, jp3 = 0;
			uint16_t pos = 0;
			DATA( &pos, 2 );
			jp1 = func->code.size;

			FUNC_ENTER;
			if( !compile_node( C, func, node->child->next ) ) goto fail;

			if( node->child->next->next )
			{
				BYTE( SI_JUMP );
				DATA( &pos, 2 );
				jp2 = func->code.size;
				AS_INT16( func->code.ptr + jp1 - 2 ) = jp2 - jp1;

				FUNC_ENTER;
				if( !compile_node( C, func, node->child->next->next ) ) goto fail;
				jp3 = func->code.size;
				AS_INT16( func->code.ptr + jp2 - 2 ) = jp3 - jp2;
			}
			else
			{
				AS_INT16( func->code.ptr + jp1 - 2 ) = func->code.size - jp1;
			}
		}
		break;

	case SFT_WHILE:
		FUNC_HIT( "WHILE" );
		C->fctx->loops++;
		i = func->code.size;
		FUNC_ENTER;
		if( !compile_node_r( C, func, node->child ) ) goto fail; /* test */
		BYTE( SI_JMPF );
		{
			int16_t off;
			int32_t jp1, jp2 = 0;
			uint16_t pos = 0;
			DATA( &pos, 2 );
			jp1 = func->code.size;

			FUNC_ENTER;
			if( !compile_node( C, func, node->child->next ) ) goto fail; /* while */

			if( !compile_breaks( C, func, 1 ) )
				goto fail;

			BYTE( SI_JUMP );
			jp2 = func->code.size + 2;
			off = i - jp2;
			DATA( &off, 2 );
			AS_INT16( func->code.ptr + jp1 - 2 ) = jp2 - jp1;
		}
		if( !compile_breaks( C, func, 0 ) )
			goto fail;
		C->fctx->loops--;
		break;

	case SFT_FOR:
		FUNC_HIT( "FOR" );
		C->fctx->loops++;
		FUNC_ENTER;
		if( !compile_node( C, func, node->child ) ) goto fail; /* init */
		i = func->code.size;
		FUNC_ENTER;
		if( !compile_node_r( C, func, node->child->next ) ) goto fail; /* test */
		BYTE( SI_JMPF );
		{
			int16_t off;
			int32_t jp1, jp2 = 0;
			uint16_t pos = 0;
			DATA( &pos, 2 );
			jp1 = func->code.size;

			FUNC_ENTER;
			if( !compile_node( C, func, node->child->next->next->next ) ) goto fail; /* block */

			if( !compile_breaks( C, func, 1 ) )
				goto fail;
			FUNC_ENTER;
			if( !compile_node( C, func, node->child->next->next ) ) goto fail; /* incr */

			BYTE( SI_JUMP );
			jp2 = func->code.size + 2;
			off = i - jp2;
			DATA( &off, 2 );
			AS_INT16( func->code.ptr + jp1 - 2 ) = jp2 - jp1;
		}
		if( !compile_breaks( C, func, 0 ) )
			goto fail;
		C->fctx->loops--;
		break;

	case SFT_BREAK:
		FUNC_HIT( "BREAK" );
		{
			int16_t off = 0;
			TokenList tl = sgsT_Next( node->token );
			int32_t blev = 1;
			if( *tl == ST_NUMINT )
				blev = (uint32_t)*(sgs_Integer*)( tl + 1 );
			if( blev > C->fctx->loops )
			{
				sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), C->fctx->loops ? "Break level too high." : "Attempted to break while not in a loop." );
				goto fail;
			}
			BYTE( SI_JUMP );
			fctx_binfo_add( C->fctx, func->code.size, C->fctx->loops + 1 - blev, FALSE );
			DATA( &off, 2 );
		}
		break;

	case SFT_CONT:
		FUNC_HIT( "CONTINUE" );
		{
			int16_t off = 0;
			TokenList tl = sgsT_Next( node->token );
			int32_t blev = 1;
			if( *tl == ST_NUMINT )
				blev = (uint32_t)*(sgs_Integer*)( tl + 1 );
			if( blev > C->fctx->loops )
			{
				sgs_Printf( C, SGS_ERROR, sgsT_LineNum( node->token ), C->fctx->loops ? "Continue level too high." : "Attempted to continue while not in a loop." );
				goto fail;
			}
			BYTE( SI_JUMP );
			fctx_binfo_add( C->fctx, func->code.size, C->fctx->loops + 1 - blev, TRUE );
			DATA( &off, 2 );
		}
		break;

	case SFT_FUNC:
		FUNC_HIT( "FUNC" );
		{
			uint16_t pos;
			sgs_FuncCtx* fctx = fctx_create(), *bkfctx = C->fctx;
			sgs_CompFunc* nf = make_compfunc();

			C->fctx = fctx;
			FUNC_ENTER;
			if( !preparse_arglist( C, nf, node->child ) ) { fctx_destroy( fctx ); C->fctx = bkfctx; goto fail; }
			preparse_varlists( C, nf, node->child->next );
			FUNC_ENTER;
			if( !compile_node( C, nf, node->child->next ) ) { fctx_destroy( fctx ); C->fctx = bkfctx; goto fail; }
#if SGS_DEBUG && SGS_DEBUG_DATA
			fctx_dump( fctx );
			sgsBC_Dump( nf );
#endif
			fctx_destroy( fctx );
			C->fctx = bkfctx;

			pos = add_const_f( C, func, nf );
			BYTE( SI_PUSHC );
			DATA( &pos, 2 );

			if( node->child->next->next )
			{
				FUNC_ENTER;
				if( !compile_node_w( C, func, node->child->next->next ) ) goto fail;
			}
		}
		break;

	case SFT_VARLIST:
	case SFT_GVLIST:
		FUNC_HIT( node->type == SFT_VARLIST ? "VARLIST" : "GLOBALVARLIST" );
		{
			FTNode* pp = node->child;
			while( pp )
			{
				if( pp->child )
				{
					if( !compile_node_r( C, func, pp->child ) ) goto fail;
					if( !pp->token || *pp->token != ST_IDENT ) goto fail;
					compile_ident_w( C, func, pp );
				}
				pp = pp->next;
			}
		}
		break;

	default:
		sgs_Printf( C, SGS_ERROR, -1, "Unexpected tree node [uncaught/internal error]." );
		goto fail;
	}

	FUNC_END;
	return 1;

fail:
	FUNC_END;
	return 0;
}


sgs_CompFunc* sgsBC_Generate( SGS_CTX, FTNode* tree )
{
	sgs_CompFunc* func = make_compfunc();
	sgs_FuncCtx* fctx = fctx_create();
	fctx->func = FALSE;
	C->fctx = fctx;
	preparse_varlists( C, func, tree );
	if( !compile_node( C, func, tree ) )
		goto fail;

	C->fctx = NULL;
#if SGS_DEBUG && SGS_DEBUG_DATA
	fctx_dump( fctx );
#endif
	fctx_destroy( fctx );
	return func;

fail:
	sgsBC_Free( C, func );
	C->fctx = NULL;
	fctx_destroy( fctx );
	C->state |= SGS_HAS_ERRORS;
	return NULL;
}

void sgsBC_Dump( sgs_CompFunc* func )
{
	sgs_Variable** vbeg = (sgs_Variable**) func->consts.ptr;
	sgs_Variable** vend = (sgs_Variable**) ( func->consts.ptr + func->consts.size );
	sgs_Variable** var = vbeg;

	printf( "{\n" );
	printf( "> constants:\n" );
	while( var < vend )
	{
		printf( "%4d = ", (int) ( var - vbeg ) );
		sgsVM_VarDump( *var );
		printf( "\n" );
		var++;
	}
	printf( "> code:\n" );
	dump_opcode( func->code.ptr, func->code.size );
	printf( "}\n" );
}

void sgsBC_Free( SGS_CTX, sgs_CompFunc* func )
{
	sgs_Variable** vbeg = (sgs_Variable**) func->consts.ptr;
	sgs_Variable** vend = (sgs_Variable**) ( func->consts.ptr + func->consts.size );
	sgs_Variable** var = vbeg;
	while( var < vend )
	{
		sgs_Release( C, *var );
		var++;
	}

	strbuf_destroy( &func->code );
	strbuf_destroy( &func->consts );
	sgs_Free( func );
}


