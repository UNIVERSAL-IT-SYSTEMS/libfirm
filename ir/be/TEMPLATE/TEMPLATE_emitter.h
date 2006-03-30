#ifndef _TEMPLATE_EMITTER_H_
#define _TEMPLATE_EMITTER_H_

#include "irargs_t.h"  // this also inlucdes <libcore/lc_print.h>
#include "irnode.h"
#include "debug.h"

#include "../bearch.h"

#include "bearch_TEMPLATE_t.h"

typedef struct _emit_env_t {
	FILE                      *out;
	const arch_env_t          *arch_env;
	const TEMPLATE_code_gen_t *cg;
	DEBUG_ONLY(firm_dbg_module_t *mod;)
} emit_env_t;

const lc_arg_env_t *TEMPLATE_get_arg_env(void);

void equalize_dest_src(FILE *F, ir_node *n);

int get_TEMPLATE_reg_nr(ir_node *irn, int posi, int in_out);
const char *get_TEMPLATE_in_reg_name(ir_node *irn, int pos);

void TEMPLATE_gen_routine(FILE *F, ir_graph *irg, const TEMPLATE_code_gen_t *cg);

#endif /* _TEMPLATE_EMITTER_H_ */
