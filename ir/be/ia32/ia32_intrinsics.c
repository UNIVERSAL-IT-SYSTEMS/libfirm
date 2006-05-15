/**
 * This file implements the mapping of 64Bit intrinsic functions to
 * code or library calls.
 * @author Michael Beck
 * $Id$
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "irgmod.h"
#include "irop.h"
#include "irnode_t.h"
#include "ircons.h"
#include "irprog_t.h"
#include "lower_intrinsics.h"
#include "lower_dw.h"
#include "mangle.h"
#include "array.h"

#include "ia32_new_nodes.h"

/** The array of all intrinsics that must be mapped. */
static i_record *intrinsics;

/** An array to cache all entities */
static entity *i_ents[iro_MaxOpcode];

/*
 * Maps all intrinsic calls that the backend support
 * and map all instructions the backend did not support
 * to runtime calls.
 */
void ia32_handle_intrinsics(void) {
	if (intrinsics && ARR_LEN(intrinsics) > 0)
		lower_intrinsics(intrinsics, ARR_LEN(intrinsics));
}

#define BINOP_Left_Low   0
#define BINOP_Left_High  1
#define BINOP_Right_Low  2
#define BINOP_Right_High 3

static void resolve_call(ir_node *call, ir_node *l_res, ir_node *h_res, ir_graph *irg, ir_node *block) {
	ir_node *res, *in[2];

	in[0] = l_res;
	in[1] = h_res;
	res = new_r_Tuple(irg, block, 2, in);

	turn_into_tuple(call, pn_Call_max);
	set_Tuple_pred(call, pn_Call_M_regular,        get_irg_no_mem(irg));
	set_Tuple_pred(call, pn_Call_X_except,         get_irg_bad(irg));
	set_Tuple_pred(call, pn_Call_T_result,         res);
	set_Tuple_pred(call, pn_Call_M_except,         get_irg_bad(irg));
	set_Tuple_pred(call, pn_Call_P_value_res_base, get_irg_bad(irg));
}

/**
 * Map an Add (a_l, a_h, b_l, b_h)
 */
static int map_Add(ir_node *call, void *ctx) {
	ir_graph *irg        = current_ir_graph;
	dbg_info *dbg        = get_irn_dbg_info(call);
	ir_node  *block      = get_nodes_block(call);
	ir_node  **params    = get_Call_param_arr(call);
	ir_type  *method     = get_Call_type(call);
	ir_node  *a_l        = params[BINOP_Left_Low];
	ir_node  *a_h        = params[BINOP_Left_High];
	ir_node  *b_l        = params[BINOP_Right_Low];
	ir_node  *b_h        = params[BINOP_Right_High];
	ir_mode  *l_res_mode = get_type_mode(get_method_res_type(method, 0));
	ir_mode  *h_res_mode = get_type_mode(get_method_res_type(method, 1));
	ir_node  *l_res, *h_res;

	/* l_res = a_l + b_l */
	l_res = new_rd_ia32_l_Add(dbg, irg, block, a_l, b_l, l_res_mode);

	/* h_res = a_h + b_h + carry */
	h_res = new_rd_ia32_l_AddC(dbg, irg, block, a_h, b_h, h_res_mode);

	resolve_call(call, l_res, h_res, irg, block);
	return 1;
}

/**
 * Map a Sub (a_l, a_h, b_l, b_h)
 */
static int map_Sub(ir_node *call, void *ctx) {
	ir_graph *irg        = current_ir_graph;
	dbg_info *dbg        = get_irn_dbg_info(call);
	ir_node  *block      = get_nodes_block(call);
	ir_node  **params    = get_Call_param_arr(call);
	ir_type  *method     = get_Call_type(call);
	ir_node  *a_l        = params[BINOP_Left_Low];
	ir_node  *a_h        = params[BINOP_Left_High];
	ir_node  *b_l        = params[BINOP_Right_Low];
	ir_node  *b_h        = params[BINOP_Right_High];
	ir_mode  *l_res_mode = get_type_mode(get_method_res_type(method, 0));
	ir_mode  *h_res_mode = get_type_mode(get_method_res_type(method, 1));
	ir_node  *l_res, *h_res;

	/* l_res = a_l - b_l */
	l_res = new_rd_ia32_l_Sub(dbg, irg, block, a_l, b_l, l_res_mode);

	/* h_res = a_h - b_h - carry */
	h_res = new_rd_ia32_l_SubC(dbg, irg, block, a_h, b_h, h_res_mode);

	resolve_call(call, l_res, h_res, irg, block);

	return 1;
}

/* Ia32 implementation of intrinsic mapping. */
entity *ia32_create_intrinsic_fkt(ir_type *method, const ir_op *op,
                                  const ir_mode *imode, const ir_mode *omode,
                                  void *context)
{
	i_record      elt;
	entity        **ent = NULL;
	i_mapper_func mapper;

	if (! intrinsics)
		intrinsics = NEW_ARR_F(i_record, 0);

	switch (get_op_code(op)) {
	case iro_Add:
		ent    = &i_ents[iro_Add];
		mapper = map_Add;
		break;
	case iro_Sub:
		ent    = &i_ents[iro_Sub];
		mapper = map_Sub;
		break;
	default:
		ir_fprintf(stderr, "FIXME: unhandled op for ia32 intrinsic function %+O\n", op);
		return def_create_intrinsic_fkt(method, op, imode, omode, context);
	}

	if (ent && ! *ent) {
#define IDENT(s)  new_id_from_chars(s, sizeof(s)-1)

		ident *id = mangle(IDENT("L"), get_op_ident(op));
		*ent = new_entity(get_glob_type(), id, method);
	}

	elt.i_call.kind     = INTRINSIC_CALL;
	elt.i_call.i_ent    = *ent;
	elt.i_call.i_mapper = mapper;
	elt.i_call.ctx      = context;
	elt.i_call.link     = NULL;

	ARR_APP1(i_record, intrinsics, elt);
	return *ent;
}
