#ifndef _arm_EMITTER_H_
#define _arm_EMITTER_H_

#include "irargs_t.h"  // this also inlucdes <libcore/lc_print.h>
#include "irnode.h"
#include "debug.h"

#include "../bearch.h"

#include "bearch_arm_t.h"

typedef struct _arm_emit_env_t {
	FILE                      *out;
	const arch_env_t          *arch_env;
	const arm_code_gen_t *cg;
	DEBUG_ONLY(firm_dbg_module_t         *mod;)
} arm_emit_env_t;

const lc_arg_env_t *arm_get_arg_env(void);

void equalize_dest_src(FILE *F, ir_node *n);

int get_arm_reg_nr(ir_node *irn, int posi, int in_out);
const char *get_arm_in_reg_name(ir_node *irn, int pos);

void arm_gen_routine(FILE *F, ir_graph *irg, const arm_code_gen_t *cg);

#endif /* _arm_EMITTER_H_ */
