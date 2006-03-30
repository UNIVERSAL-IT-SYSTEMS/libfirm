#ifndef _BEARCH_mips_T_H_
#define _BEARCH_mips_T_H_

#include "debug.h"
#include "irgopt.h"
#include "bearch_mips.h"
#include "mips_nodes_attr.h"
#include "../be.h"
#include "set.h"

struct _mips_code_gen_t {
	const arch_code_generator_if_t *impl;           /**< implementation */
	ir_graph                       *irg;            /**< current irg */
	FILE                           *out;            /**< output file */
	const arch_env_t               *arch_env;       /**< the arch env */
	set                            *reg_set;        /**< set to memorize registers for FIRM nodes (e.g. phi) */
	int                             emit_decls;     /**< flag indicating if decls were already emitted */
	const be_irg_t                 *birg;           /**< The be-irg (contains additional information about the irg) */
	ir_node                        **bl_list;		/**< The block schedule list. */
	survive_dce_t				   *bl_list_sdce;	/**< survive dce environment for the block schedule list */
	DEBUG_ONLY(firm_dbg_module_t   *mod;)           /**< debugging module */
};


typedef struct _mips_isa_t {
	const arch_isa_if_t   *impl;
	const arch_register_t *sp;            /**< The stack pointer register. */
	const arch_register_t *fp;            /**< The base pointer register. */
	const int              stack_dir;     /**< -1 for decreasing, 1 for increasing. */
	int                  num_codegens;
} mips_isa_t;


typedef struct _mips_irn_ops_t {
	const arch_irn_ops_if_t *impl;
	mips_code_gen_t     *cg;
} mips_irn_ops_t;


/* this is a struct to minimize the number of parameters
   for transformation walker */
typedef struct _mips_transform_env_t {
	dbg_info          *dbg;      /**< The node debug info */
	ir_graph          *irg;      /**< The irg, the node should be created in */
	ir_node           *block;    /**< The block, the node should belong to */
	ir_node           *irn;      /**< The irn, to be transformed */
	ir_mode           *mode;     /**< The mode of the irn */
	mips_code_gen_t   *cg;       /**< The code generator */
	DEBUG_ONLY(firm_dbg_module_t *mod;) /**< The firm debugger */
} mips_transform_env_t;

ir_node *mips_new_NoReg(mips_code_gen_t *cg);

#endif /* _BEARCH_mips_T_H_ */
