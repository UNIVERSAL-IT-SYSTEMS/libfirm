/**
 * Author:      Daniel Grund
 * Date:		17.05.2005
 * Copyright:   (c) Universitaet Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#include "irprog.h"

#include <lpp/lpp.h>
#include <lpp/lpp_net.h>
#include "xmalloc.h"
#include "becopyopt.h"
#include "becopystat.h"
#include "besched_t.h"

#define LPP_HOST "i44pc52"
#define LPP_SOLVER "cplex"

#undef DUMP_MPS
static firm_dbg_module_t *dbg = NULL;

#define MAX(a,b) ((a<b)?(b):(a))
#define EPSILON 0.00001
#define SLOTS_LIVING 32

typedef struct _simpl_t {
	struct list_head chain;
	if_node_t *ifn;
} simpl_t;

typedef struct _problem_instance_t {
	const copy_opt_t *co;			/** the copy_opt problem */
	/* problem size reduction removing simple nodes */
	struct list_head simplicials;	/**< holds all simpl_t's in right order to color*/
	pset *removed;					/**< holds all removed simplicial irns */
	/* lp problem */
	lpp_t *dilp;					/**< problem formulation directly as milp */
	/* overhead stuff */
	lpp_t *curr_lp;					/**< points to the problem currently used */
	int cst_counter, last_x_var;
	char buf[32];
	int all_simplicial;
} problem_instance_t;

#define is_removed(irn) pset_find_ptr(pi->removed, irn)

#define is_color_possible(irn,color) arch_reg_is_allocatable(get_arch_env(pi->co), irn, arch_pos_make_out(0), arch_register_for_index(pi->co->chordal_env->cls, color))

/*
 * Some stuff for variable name handling.
 */
#define mangle_cst(buf, prefix, nr) \
			snprintf((buf), sizeof(buf), "%c%d", (prefix), (nr))

#define mangle_var(buf, prefix, node_nr, color) \
			snprintf((buf), sizeof(buf), "%c%d_%d", (prefix), (node_nr), (color))

#define mangle_var_irn(buf, prefix, irn, color) \
			mangle_var((buf), (prefix), get_irn_graph_nr(irn), (color))

#define split_var(var, nnr, col) \
			sscanf(var, "x%d_%d", (nnr), (col))


/**
 * Checks if a node is simplicial in the graph
 * heeding the already removed nodes.
 */
static INLINE int pi_is_simplicial(problem_instance_t *pi, const if_node_t *ifn) {
	int i, o, size = 0;
	if_node_t **all, *curr;
	all = alloca(ifn_get_degree(ifn) * sizeof(*all));

	/* get all non-removed neighbors */
	foreach_neighb(ifn, curr)
		if (!is_removed(curr))
			all[size++] = curr;

	/* check if these form a clique */
	for (i=0; i<size; ++i)
		for (o=i+1; o<size; ++o)
			if (!ifg_has_edge(pi->co->chordal_env, all[i], all[o]))
				return 0;

	/* all edges exist so this is a clique */
	return 1;
}

/**
 * Iterative finds and 'removes' from the graph all nodes which are
 * simplicial AND not member of a equal-color-wish
 */
static void pi_find_simplicials(problem_instance_t *pi) {
	set *if_nodes;
	if_node_t *ifn;
	int redo = 1;

	DBG((dbg, LEVEL_2, "Find simlicials...\n"));

	if_nodes = be_ra_get_ifg_nodes(pi->co->chordal_env);
	while (redo) {
		redo = 0;
		for (ifn = set_first(if_nodes); ifn; ifn = set_next(if_nodes)) {
			ir_node *irn = get_irn_for_graph_nr(get_irg(pi->co), ifn->nnr);
			if (!is_removed(irn) && !is_optimizable(get_arch_env(pi->co), irn) &&
          !is_optimizable_arg(pi->co, irn) && pi_is_simplicial(pi, ifn)) {
				simpl_t *s = xmalloc(sizeof(*s));
				s->ifn = ifn;
				list_add(&s->chain, &pi->simplicials);
				pset_insert_ptr(pi->removed, irn);
				redo = 1;
				DBG((dbg, LEVEL_2, " Removed %n %d\n", irn, get_irn_graph_nr(irn)));
			}
		}
	}
	if (set_count(be_ra_get_ifg_nodes(pi->co->chordal_env)) == pset_count(pi->removed))
		pi->all_simplicial = 1;
}

/**
 * Add coloring-force conditions
 * Matrix A: knapsack constraint for each node
 */
static void pi_add_constr_A(problem_instance_t *pi) {
	pmap_entry *pme;

	DBG((dbg, LEVEL_2, "Add A constraints...\n"));
	/* iterate over all blocks */
	pmap_foreach(pi->co->chordal_env->border_heads, pme) {
		struct list_head *head = pme->value;
		border_t *curr;
		bitset_t *pos_regs = bitset_alloca(pi->co->chordal_env->cls->n_regs);

		list_for_each_entry_reverse(border_t, curr, head, list)
			if (curr->is_def && curr->is_real && !is_removed(curr->irn)) {
				int cst_idx, nnr, col;

				nnr = get_irn_graph_nr(curr->irn);
				mangle_cst(pi->buf, 'A', nnr);
				cst_idx = lpp_add_cst(pi->curr_lp, pi->buf, lpp_equal, 1);

				// iterate over all possible colors in order
				bitset_clear_all(pos_regs);
				arch_get_allocatable_regs(get_arch_env(pi->co), curr->irn, arch_pos_make_out(0), pi->co->chordal_env->cls, pos_regs);
				bitset_foreach(pos_regs, col) {
					int var_idx;
					mangle_var(pi->buf, 'x', nnr, col);
					var_idx = lpp_add_var(pi->curr_lp, pi->buf, lpp_binary, 0);
					pi->last_x_var = var_idx;
					lpp_set_factor_fast(pi->curr_lp, cst_idx, var_idx, 1);
				}
			}
	}
}

/**
 * Checks if all nodes in @p living are live in in block @p block.
 * @return 1 if all are live in
 *         0 else
 */
static INLINE int all_live_in(ir_node *block, pset *living) {
	ir_node *n;
	for (n = pset_first(living); n; n = pset_next(living))
		if (!is_live_in(block, n)) {
			pset_break(living);
			return 0;
		}
	return 1;
}

/**
 * Finds cliques in the interference graph, considering only nodes
 * for which the color @p color is possible. Finds only 'maximal-cliques',
 * viz cliques which are not contained in another one.
 * Matrix B: interference constraints using cliques
 */
static void pi_add_constr_B(problem_instance_t *pi, int color) {
	enum phase_t {growing, shrinking} phase = growing;
	border_t *b;
	pmap_entry *pme;
	pset *living = pset_new_ptr(SLOTS_LIVING);

	DBG((dbg, LEVEL_2, "Add B constraints (col = %d)...\n", color));
	/* iterate over all blocks */
	pmap_foreach(pi->co->chordal_env->border_heads, pme) {
		ir_node *block = pme->key;
		struct list_head *head = pme->value;

		list_for_each_entry_reverse(border_t, b, head, list) {
			const ir_node *irn = b->irn;
			if (is_removed(irn) || !is_color_possible(irn, color))
				continue;

			if (b->is_def) {
				DBG((dbg, LEVEL_2, "Def %n\n", irn));
				pset_insert_ptr(living, irn);
				phase = growing;
			} else { /* is_use */
				DBG((dbg, LEVEL_2, "Use %n\n", irn));

				/* before shrinking the set, store the current 'maximum' clique;
				 * do NOT if clique is a single node
				 * do NOT if all values are live_in (in this case they were contained in a live-out clique elsewhere) */
				if (phase == growing && pset_count(living) >= 2 && !all_live_in(block, living)) {
					int cst_idx;
					ir_node *n;
					mangle_cst(pi->buf, 'B', pi->cst_counter);
					cst_idx = lpp_add_cst(pi->curr_lp, pi->buf, lpp_less, 1);
					for (n = pset_first(living); n; n = pset_next(living)) {
						int var_idx;
						mangle_var_irn(pi->buf, 'x', n, color);
						var_idx = lpp_get_var_idx(pi->curr_lp, pi->buf);
						lpp_set_factor_fast(pi->curr_lp, cst_idx, var_idx, 1);
					}
					pi->cst_counter++;
				}
				pset_remove_ptr(living, irn);
				phase = shrinking;
			}
		}
	}
	assert(0 == pset_count(living));
	del_pset(living);
}

/**
 * Generates constraints which interrelate x with y variables.
 * x1 and x2 have the different colors ==> y_12 = 1
 */
static void pi_add_constr_E(problem_instance_t *pi) {
	unit_t *curr;
	bitset_t *root_regs, *arg_regs, *work_regs;
	int cst_counter = 0;
	unsigned nregs = pi->co->chordal_env->cls->n_regs;
	root_regs = bitset_alloca(nregs);
	arg_regs = bitset_alloca(nregs);
	work_regs = bitset_alloca(nregs);

	DBG((dbg, LEVEL_2, "Add E constraints...\n"));
	/* for all roots of optimization units */
	list_for_each_entry(unit_t, curr, &pi->co->units, units) {
		ir_node *root, *arg;
		int rootnr, argnr, color;
		int y_idx, i;
		char buf[32];

		root = curr->nodes[0];
		rootnr = get_irn_graph_nr(root);
		bitset_clear_all(root_regs);
		arch_get_allocatable_regs(get_arch_env(pi->co), root, arch_pos_make_out(0), pi->co->chordal_env->cls, root_regs);

		/* for all arguments of root */
		for (i = 1; i < curr->node_count; ++i) {
			arg = curr->nodes[i];
			argnr = get_irn_graph_nr(arg);
			bitset_clear_all(arg_regs);
			arch_get_allocatable_regs(get_arch_env(pi->co), arg, arch_pos_make_out(0), pi->co->chordal_env->cls, arg_regs);

			/* Introduce new variable and set factor in objective function */
			mangle_var(buf, 'y', rootnr, argnr);
			y_idx = lpp_add_var(pi->curr_lp, buf, lpp_continous, curr->costs[i]);

			//BETTER: y vars as binary or continous	vars ??
			/* set starting value */
			//lpp_set_start_value(pi->curr_lp, y_idx, (get_irn_col(pi->co, root) != get_irn_col(pi->co, arg)));

			/* For all colors root and arg have in common, add 2 constraints to E */
			bitset_copy(work_regs, root_regs);
			bitset_and(work_regs, arg_regs);
			bitset_foreach(work_regs, color) {
				int root_idx, arg_idx, cst_idx;
				mangle_var(buf, 'x', rootnr, color);
				root_idx = lpp_get_var_idx(pi->curr_lp, buf);
				mangle_var(buf, 'x', argnr, color);
				arg_idx = lpp_get_var_idx(pi->curr_lp, buf);

				/* add root-arg-y <= 0 */
				mangle_cst(buf, 'E', cst_counter++);
				cst_idx = lpp_add_cst(pi->curr_lp, buf, lpp_less, 0);
				lpp_set_factor_fast(pi->curr_lp, cst_idx, root_idx, 1);
				lpp_set_factor_fast(pi->curr_lp, cst_idx, arg_idx, -1);
				lpp_set_factor_fast(pi->curr_lp, cst_idx, y_idx, -1);

				/* add arg-root-y <= 0 */
				mangle_cst(buf, 'E', cst_counter++);
				cst_idx = lpp_add_cst(pi->curr_lp, buf, lpp_less, 0);
				lpp_set_factor_fast(pi->curr_lp, cst_idx, root_idx, -1);
				lpp_set_factor_fast(pi->curr_lp, cst_idx, arg_idx, 1);
				lpp_set_factor_fast(pi->curr_lp, cst_idx, y_idx, -1);
			}
			/* For all colors root and arg have "disjunct", add 1 constraints to E.
			 * If root gets a color the arg is not possible to get then they will
			 * definetly get different colors. So y has to be 1.
			 * Vice versa for arg.
			 */
			bitset_copy(work_regs, root_regs);
			bitset_xor(work_regs, arg_regs);
			bitset_foreach(work_regs, color) {
				int root_idx, arg_idx, cst_idx;
				mangle_var(buf, 'x', rootnr, color);
				root_idx = lpp_get_var_idx(pi->curr_lp, buf);
				mangle_var(buf, 'x', argnr, color);
				arg_idx = lpp_get_var_idx(pi->curr_lp, buf);

				mangle_cst(buf, 'E', cst_counter++);
				cst_idx = lpp_add_cst(pi->curr_lp, buf, lpp_less, 0);
				if (bitset_is_set(root_regs, color)) {
					/* add root-y <= 0 */
					lpp_set_factor_fast(pi->curr_lp, cst_idx, root_idx, 1);
					lpp_set_factor_fast(pi->curr_lp, cst_idx, y_idx, -1);
				} else {
					assert(bitset_is_set(arg_regs, color) && "bitset_xor is buggy");
					/* add arg-y <= 0 */
					lpp_set_factor_fast(pi->curr_lp, cst_idx, arg_idx, 1);
					lpp_set_factor_fast(pi->curr_lp, cst_idx, y_idx, -1);
				}
			}
		}
	}
}

/**
 * Matrix S: maximum independent set constraints
 * Generates lower bound-cuts for optimization units with inner interferences.
 * Sum(y_{root, arg}, arg \in Args) <= max_indep_set_size - 1
 */
static void pi_add_constr_S(problem_instance_t *pi) {
	unit_t *curr;
	int cst_counter = 0;
	DBG((dbg, LEVEL_2, "Add S constraints...\n"));

	/* for all optimization units */
	list_for_each_entry(unit_t, curr, &pi->co->units, units) {
		const ir_node *root, *arg;
		int rootnr, argnr;
		int cst_idx, y_idx, i;
		char buf[32];

		if (curr->min_nodes_costs == 0)
			continue;

		root = curr->nodes[0];
		rootnr = get_irn_graph_nr(root);
		mangle_cst(buf, 'S', cst_counter++);
		cst_idx = lpp_add_cst(pi->curr_lp, buf, lpp_greater, curr->min_nodes_costs);

		/* for all arguments */
		for (i = 1; i < curr->node_count; ++i) {
			arg = curr->nodes[i];
			argnr = get_irn_graph_nr(arg);
			mangle_var(buf, 'y', rootnr, argnr);
			y_idx = lpp_get_var_idx(pi->curr_lp, buf);
			lpp_set_factor_fast(pi->curr_lp, cst_idx, y_idx, curr->costs[i]);
		}
	}
}

static INLINE int get_costs(problem_instance_t *pi, ir_node *phi, ir_node *irn) {
	int i;
	unit_t *curr;
	/* search optimization unit for phi */
	list_for_each_entry(unit_t, curr, &pi->co->units, units)
		if (curr->nodes[0] == phi) {
			for (i=1; i<curr->node_count; ++i)
				if (curr->nodes[i] == irn)
					return curr->costs[i];
			assert(0 && "irn must occur in this ou");
		}
	assert(0 && "phi must be found in a ou");
	return 0;
}

static void M_constr_walker(ir_node *block, void *env) {
	problem_instance_t *pi = env;
	int count, arity, row, col, other_row, *costs;
	ir_node **phis, *phi, *irn, **phi_matrix;
	pset *done;
	bitset_t *candidates;

	/* Count all phi nodes of this block */
	for (count=0, irn = sched_first(block); is_Phi(irn); irn = sched_next(irn))
		count++;

	/* We at least 2 phi nodes for this class of inequalities */
	if (count < 2)
		return;

	/* Build the \Phi-Matrix */
	arity = get_irn_arity(sched_first(block));
	phis = alloca(count * sizeof(*phis));
	costs = alloca(count * sizeof(costs));
	phi_matrix = alloca(count*arity * sizeof(*phi_matrix));
	candidates = bitset_alloca(count);

	phi = sched_first(block);
	for (row=0; row<count; ++row) {
		phis[row] = phi;
		for (col=0; col<arity; ++col) {
			ir_node *arg = get_irn_n(phi, col);
			/* Sort out all arguments interfering with its phi */
			if (nodes_interfere(pi->co->chordal_env, phi, arg)) {
				//TODO remove next line
				printf("\n\n\n Sorted out an entry. Report this to Daniel.\n\n\n");
				phi_matrix[row*arity + col] =  NULL;
			} else
				phi_matrix[row*arity + col] =  arg;
		}
		phi = sched_next(phi);
	}

	/* Now find the interesting patterns in the matrix:
	 * All nodes which are used at least twice in a column. */
	/* columnwise ... */
	for (col=0; col<arity; ++col) {
		done = pset_new_ptr_default();
		for (row=0; row<count; ++row) {
			irn = phi_matrix[row*arity + col];
			/*
			 * is this an interfering arg (NULL)
			 * or has the irn already been processed in this col?
			 */
			if (!irn || pset_find_ptr(done, irn))
				continue;
			else
				pset_insert_ptr(done, irn);

			/* insert irn in candidates */
			bitset_clear_all(candidates);
			bitset_set(candidates, row);
			/* search the irn in the rows below */
			for (other_row = row+1; other_row<count; ++other_row)
				if (irn == phi_matrix[other_row*arity + col]) {
					/* found the irn in the same col in another row */
					bitset_set(candidates, other_row);
				}

			/* now we know all occurences of irn in this col */
			if (bitset_popcnt(candidates) < 2)
				continue;

			/* compute the minimal costs (rhs) */
			int phi_nr, sum=0, max=-1, minimal_costs;
			bitset_foreach(candidates, phi_nr) {
				costs[phi_nr] = get_costs(pi, phis[phi_nr], irn);
				sum += costs[phi_nr];
				max = MAX(max, costs[phi_nr]);
			}
			minimal_costs = sum - max;

			/* generate an unequation finally.
			 * phis are indexed in the bitset,
			 * shared argument is irn
			 * rhs is minimal_costs */
			{
				char buf[32];
				ir_node *root;
				int pos, irnnr, rootnr, cst_idx, y_idx, cst_counter = 0;

				irnnr = get_irn_graph_nr(irn);
				mangle_cst(buf, 'M', cst_counter++);
				cst_idx = lpp_add_cst(pi->curr_lp, buf, lpp_greater, minimal_costs);

				/* for all phis */
				bitset_foreach(candidates, pos) {
					root = phis[pos];
					rootnr = get_irn_graph_nr(root);
					mangle_var(buf, 'y', rootnr, irnnr);
					y_idx = lpp_get_var_idx(pi->curr_lp, buf);
					lpp_set_factor_fast(pi->curr_lp, cst_idx, y_idx, costs[pos]);
				}
			}
		}
		del_pset(done); /* clear set for next row */
	} /*next col*/
}

/**
 * Matrix M: Multi-Arg-Use. Interrelates different \phi-functions
 * in the same block, iff they use the same arg at the same pos.
 * Only one of the phis can get the arg.
 */
static void pi_add_constr_M(problem_instance_t *pi) {
	dom_tree_walk_irg(get_irg(pi->co), M_constr_walker, NULL, pi);
}

/**
 * Generate the initial problem matrices and vectors.
 */
static problem_instance_t *new_pi(const copy_opt_t *co) {
	problem_instance_t *pi;
	int col;

	DBG((dbg, LEVEL_2, "Generating new instance...\n"));
	pi = xcalloc(1, sizeof(*pi));
	pi->co = co;
	pi->removed = pset_new_ptr_default();
	INIT_LIST_HEAD(&pi->simplicials);
	pi->dilp = new_lpp(co->name, lpp_minimize);
	pi->last_x_var = -1;

	/* problem size reduction */
	pi_find_simplicials(pi);
	//BETTER If you wish to see it: dump_ifg_w/o_removed
	if (pi->all_simplicial)
		return pi;

	/* built objective abd constraints */
	pi->curr_lp = pi->dilp;
	pi_add_constr_A(pi);
	for (col = 0; col < pi->co->chordal_env->cls->n_regs; ++col)
		pi_add_constr_B(pi, col);
	pi_add_constr_E(pi);
	pi_add_constr_S(pi);
	pi_add_constr_M(pi);

	return pi;
}

/**
 * Clean the problem instance
 */
static void free_pi(problem_instance_t *pi) {
	simpl_t *simpl, *tmp;

	DBG((dbg, LEVEL_2, "Free instance...\n"));
	free_lpp(pi->dilp);
	list_for_each_entry_safe(simpl_t, simpl, tmp, &pi->simplicials, chain)
		free(simpl);
	del_pset(pi->removed);
	free(pi);
}

/**
 * Set starting values for the mip problem according
 * to the current coloring of the graph.
 */
static void pi_set_start_sol(problem_instance_t *pi) {
	int i;
	char var_name[64];
	DBG((dbg, LEVEL_2, "Set start solution...\n"));
	for (i=1; i<=pi->last_x_var; ++i) {
		int nnr, col;
		double val;
		/* get variable name */
		lpp_get_var_name(pi->curr_lp, i, var_name, sizeof(var_name));
		/* split into components */
		if (split_var(var_name, &nnr, &col) == 2) {
			assert(get_irn_col(pi->co, get_irn_for_graph_nr(get_irg(pi->co), nnr)) != -1);
			val = (get_irn_col(pi->co, get_irn_for_graph_nr(get_irg(pi->co), nnr)) == col) ? 1 : 0;
			lpp_set_start_value(pi->curr_lp, i, val);
		} else {
			fprintf(stderr, "Variable name is: %s\n", var_name);
			assert(0 && "x vars always look like this 'x123_45'");
		}
	}
}

/**
 * Invoke a solver
 */
static void pi_solve_ilp(problem_instance_t *pi) {
	pi_set_start_sol(pi);
	lpp_solve_net(pi->curr_lp, LPP_HOST, LPP_SOLVER);
}

/**
 * Set the color of all simplicial nodes removed form
 * the graph before transforming it to an ilp.
 */
static void pi_set_simplicials(problem_instance_t *pi) {
	simpl_t *simpl, *tmp;
	bitset_t *used_cols = bitset_alloca(arch_register_class_n_regs(pi->co->chordal_env->cls));

	DBG((dbg, LEVEL_2, "Set simplicials...\n"));
	/* color the simplicial nodes in right order */
	list_for_each_entry_safe(simpl_t, simpl, tmp, &pi->simplicials, chain) {
		int free_col;
		ir_node *other_irn, *irn;
		if_node_t *other, *ifn;

		/* get free color by inspecting all neighbors */
		ifn = simpl->ifn;
		irn = get_irn_for_graph_nr(get_irg(pi->co), ifn->nnr);
		bitset_clear_all(used_cols);
		foreach_neighb(ifn, other) {
			other_irn = get_irn_for_graph_nr(get_irg(pi->co), other->nnr);
			if (!is_removed(other_irn)) /* only inspect nodes which are in graph right now */
				bitset_set(used_cols, get_irn_col(pi->co, other_irn));
		}

		/* now all bits not set are possible colors */
		free_col = bitset_next_clear(used_cols, 0);
		assert(free_col != -1 && "No free color found. This can not be.");
		set_irn_col(pi->co, irn, free_col);
		pset_remove_ptr(pi->removed, irn); /* irn is back in graph again */
	}
}

/**
 * Sets the colors of irns according to the values of variables
 * provided by the solution of the solver.
 */
static void pi_apply_solution(problem_instance_t *pi) {
	int i;
	double *sol;
	lpp_sol_state_t state;
	DBG((dbg, LEVEL_2, "Applying solution...\n"));

#ifdef DO_STAT
	curr_vals[I_ILP_ITER] += lpp_get_iter_cnt(pi->curr_lp);
	curr_vals[I_ILP_TIME] += lpp_get_sol_time(pi->curr_lp);
#endif

	sol = xmalloc((pi->last_x_var+1) * sizeof(*sol));
	state = lpp_get_solution(pi->curr_lp, sol, 1, pi->last_x_var);
	if (state != lpp_optimal) {
		printf("Solution state is not 'optimal': %d\n", state);
		assert(state >= lpp_feasible && "The solution should at least be feasible!");
	}
	for (i=0; i<pi->last_x_var; ++i) {
		int nnr, col;
		char var_name[64];

		if (sol[i] > 1-EPSILON) { /* split varibale name into components */
			lpp_get_var_name(pi->curr_lp, 1+i, var_name, sizeof(var_name));
			if (split_var(var_name, &nnr, &col) == 2) {
				DBG((dbg, LEVEL_2, "Irn %n  Idx %d  Var %s  Val %f\n", get_irn_for_graph_nr(get_irg(pi->co), nnr), i, var_name, sol[i]));
				DBG((dbg, LEVEL_2, "x%d = %d\n", nnr, col));
				set_irn_col(pi->co, get_irn_for_graph_nr(get_irg(pi->co), nnr), col);
			} else
				assert(0 && "This should be a x-var");
		}
	}
}

void co_ilp_opt(copy_opt_t *co) {
	problem_instance_t *pi;

	dbg = firm_dbg_register("ir.be.copyoptilp");
	if (!strcmp(co->name, DEBUG_IRG))
		firm_dbg_set_mask(dbg, DEBUG_IRG_LVL_ILP);
	else
		firm_dbg_set_mask(dbg, DEBUG_LVL_ILP);

	pi = new_pi(co);
	if (!pi->all_simplicial) {
#ifdef DUMP_MPS
		char buf[512];
		snprintf(buf, sizeof(buf), "%s.mps", co->name);
		lpp_dump(pi->curr_lp, buf);
#endif
		pi_solve_ilp(pi);
		pi_apply_solution(pi);
		pi_set_simplicials(pi);
	}
	free_pi(pi);
}
