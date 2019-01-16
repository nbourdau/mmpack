/*
 * @mindmaze_header@
 */
#if defined (HAVE_CONFIG_H)
# include <config.h>
#endif

#include <assert.h>
#include <mmerrno.h>
#include <stdio.h>

#include "action-solver.h"
#include "context.h"
#include "package-utils.h"
#include "utils.h"

enum {
	DONE,
	CONTINUE,
};

enum solver_state {
	VALIDATION,
	SELECTION,
	INSTALL_DEPS,
	NEXT,
	BACKTRACK,
};

/**
 * struct proc_frame - processing data frame
 * @ipkg:       index of package currently selected for installation
 * @state:      next step to perform
 * @dep:        current compiled dependency being processed
 *
 * This structure hold the data to keep track the processing of one node
 * while walking in the directed graph that represents the binary index.
 * For example, when installing a package, its dependencies must be
 * inspected. The struct proc_frame keep track which dependencies is being
 * inspected. If one on the dependences must installed a new struct
 * proc_frame is created and must be stacked.
 */
struct proc_frame {
	int ipkg;
	enum solver_state state;
	struct compiled_dep* dep;
};


/**
 * struct decision_state - snapshot of processing state at decision time
 * @last_decstate_sz:   previous top index of decstate_store of solver
 * @ops_stack_size:     top index of ops_stack of solver
 * @curr_frame:         current processing data frame
 * @pkg_index:          index of chosen package in dependency being processed
 * @num_proc_frame:     current depth of processing stack
 * @proc_frames:        content of processing stack (length @num_proc_frame)
 *
 * Structure representing a snapshot of the internal data of solver needed
 * to go back at the time of previous decision.
 */
struct decision_state {
	size_t last_decstate_sz;
	size_t ops_stack_size;
	struct proc_frame curr_frame;
	int pkg_index;
	int num_proc_frame;
	struct proc_frame proc_frames[];
};


/**
 * struct planned_op - data representing a change in inst_lut and stage_lut
 * @action:     type of action: STAGE, INSTALL or REMOVE
 * @id:         package name id involved by the change
 * @pkg:        pointer to package installed or removed
 */
struct planned_op {
	enum {STAGE, INSTALL, REMOVE} action;
	int id;
	struct mmpkg* pkg;
};


/**
 * struct solver - solver context
 * @binindex:   binary index used to inspect dependencies
 * @inst_lut:   lookup table of installed package
 * @stage_lut:  lookup table of package staged to be installed
 * @processing_stack: stack of processing frames
 * @decstate_store:   previous decision states store
 * @last_decstate_sz: decision state store size before last decision
 * @ops_stack:  stack of planned operations
 * @num_proc_frame:   current depth of processing stack
 */
struct solver {
	struct binindex* binindex;
	struct mmpkg** inst_lut;
	struct mmpkg** stage_lut;
	struct buffer processing_stack;
	struct buffer decstate_store;
	size_t last_decstate_sz;
	struct buffer ops_stack;
	int num_proc_frame;
};


/**************************************************************************
 *                                                                        *
 *                             Action stack                               *
 *                                                                        *
 **************************************************************************/
#define DEFAULT_STACK_SZ 10
LOCAL_SYMBOL
struct action_stack * mmpack_action_stack_create(void)
{
	size_t stack_size;
	struct action_stack * stack;

	stack_size = sizeof(*stack) + DEFAULT_STACK_SZ * sizeof(*stack->actions);
	stack = mm_malloc(stack_size);
	memset(stack, 0, stack_size);
	stack->size = DEFAULT_STACK_SZ;

	return stack;
}


LOCAL_SYMBOL
void mmpack_action_stack_destroy(struct action_stack * stack)
{
	int i;

	if (!stack)
		return;

	for (i = 0; i < stack->index; i++)
		mmstr_free(stack->actions[i].pathname);

	free(stack);
}


static
struct action_stack * mmpack_action_stack_push(struct action_stack * stack,
                                                      action action,
                                                      struct mmpkg const * pkg)
{
	/* increase by DEFAULT_STACK_SZ if full */
	if ((stack->index + 1) == stack->size) {
		size_t stack_size = sizeof(*stack) + (stack->size + DEFAULT_STACK_SZ) * sizeof(*stack->actions);
		stack = mm_realloc(stack, stack_size);
	}

	stack->actions[stack->index] = (struct action) {
		.action = action,
		.pkg = pkg,
	};
	stack->index++;

	return stack;
}


LOCAL_SYMBOL
struct action * mmpack_action_stack_pop(struct action_stack * stack)
{
	struct action * action;

	if (stack->index == 0)
		return NULL;

	stack->index --;
	action = &stack->actions[stack->index];

	return action;
}



/**************************************************************************
 *                                                                        *
 *                            solver context                              *
 *                                                                        *
 **************************************************************************/


static
void solver_init(struct solver* solver, struct mmpack_ctx* ctx)
{
	size_t size;

	*solver = (struct solver) {.binindex = &ctx->binindex};

	size = ctx->binindex.num_pkgname * sizeof(*solver->inst_lut);
	solver->inst_lut = mm_malloc(size);
	solver->stage_lut = mm_malloc(size);

	install_state_fill_lookup_table(&ctx->installed, &ctx->binindex,
	                                solver->inst_lut);
	memset(solver->stage_lut, 0, size);
	buffer_init(&solver->processing_stack);
	buffer_init(&solver->decstate_store);
	buffer_init(&solver->ops_stack);
}


static
void solver_deinit(struct solver* solver)
{
	buffer_deinit(&solver->ops_stack);
	buffer_deinit(&solver->decstate_store);
	buffer_deinit(&solver->processing_stack);
	free(solver->inst_lut);
	free(solver->stage_lut);
}


/**
 * solver_revert_planned_ops() - undo latest actions up to a previous state
 * @solver:     solver context to update
 * @prev_size:  size of the ops_stack up to which action must be undone
 *
 * This function undo the actions stored in the planned operation stack in
 * @solver from top to @prev_size. After this function, @solver->stage_lut
 * and solver->inst_lut will be the same as it was when @prev_size was the
 * actual size of @solver->ops_stack.
 */
static
void solver_revert_planned_ops(struct solver* solver, size_t prev_size)
{
	struct buffer* ops_stack = &solver->ops_stack;
	struct planned_op op;

	while (ops_stack->size > prev_size) {
		buffer_pop(ops_stack, &op, sizeof(op));

		switch (op.action) {
		case STAGE:
			solver->stage_lut[op.id] = NULL;
			break;

		case INSTALL:
			solver->inst_lut[op.id] = NULL;
			break;

		case REMOVE:
			solver->inst_lut[op.id] = op.pkg;
			break;

		default:
			mm_crash("Unexpected action type: %i", op.action);
		}
	}
}


/**
 * solver_stage_pkg_install() - mark a package intended to be installed
 * @solver:     solver context to update
 * @id:         package name id
 * @pkg:        pointer to package intended to be installed
 *
 * This function will update the lookup table of staged package in @solver
 * and will register the change in @solver->ops_stack.
 */
static
void solver_stage_pkg_install(struct solver* solver, int id,
                              struct mmpkg* pkg)
{
	struct planned_op op = {.action = STAGE, .pkg = pkg, .id = id};

	solver->stage_lut[id] = pkg;
	buffer_push(&solver->ops_stack, &op, sizeof(op));
}


/**
 * solver_commit_pkg_install() - register the package install operation
 * @solver:     solver context to update
 * @id:         package name id
 *
 * This function will update the lookup table of install package in @solver
 * and will register the change in @solver->ops_stack. This function can
 * only be called after solver_stage_pkg_install() has been called for the
 * same @id.
 */
static
void solver_commit_pkg_install(struct solver* solver, int id)
{
	struct mmpkg* pkg = solver->stage_lut[id];
	struct planned_op op = {.action = INSTALL, .pkg = pkg, .id = id};

	solver->inst_lut[id] = pkg;
	buffer_push(&solver->ops_stack, &op, sizeof(op));
}


/**
 * solver_save_decision_state() - save solver state associated with a decision
 * @solver:     solver context to update
 * @frame:      pointer to the current processing frame
 *
 * This function is called when a choice is presented to the solver (to
 * choose installing a package or another). It stores the necessary
 * information of @solver to possibly backtrack on the decision later.
 */
static
void solver_save_decision_state(struct solver* solver,
                                struct proc_frame* frame)
{
	struct decision_state* state;
	struct proc_frame* proc_frames = solver->processing_stack.base;
	size_t state_sz;
	int i, nframe;

	// There is no point of saving decision since there is no
	// alternative anymore
	if (frame->ipkg >= frame->dep->num_pkg-1)
		return;

	// Reserve the data to store the whole state at decision point
	nframe = solver->num_proc_frame;
	state_sz = sizeof(*state) + nframe * sizeof(*proc_frames);
	state = buffer_reserve_data(&solver->decstate_store, state_sz);

	// Save all state of @solver needed to restore processing at the
	// moment of the decision
	state->last_decstate_sz = solver->last_decstate_sz;
	state->ops_stack_size = solver->ops_stack.size;
	state->num_proc_frame = nframe;
	state->curr_frame = *frame;
	for (i = 0; i < nframe; i++)
		state->proc_frames[i] = proc_frames[i];

	buffer_inc_size(&solver->decstate_store, state_sz);
	solver->last_decstate_sz = state_sz;
}


/**
 * solver_backtrack_on_decision() - revisit decision and restore state
 * @solver:     solver context to update
 * @frame:      pointer to processing frame
 *
 * This function should be called when a it has been realized that
 * constraints are not satisfiable. It restores the state saved at the
 * latest decision point and pick the next decision alternative.
 *
 * Return: an non-negative package index corresponding to the new package
 * alternative to try. If the return value is negative, there is no more
 * decision to revisit, hence it means that the global requirements are not
 * satisfiable.
 */
static
int solver_backtrack_on_decision(struct solver* solver,
                                 struct proc_frame* frame)
{
	struct decision_state* state;
	struct proc_frame* proc_frames = solver->processing_stack.base;
	int i, nframe;

	// If decision stack is empty, the overall problem is not satisfiable
	if (solver->last_decstate_sz == 0)
		return -1;

	state = buffer_dec_size(&solver->decstate_store, solver->last_decstate_sz);

	solver->last_decstate_sz = state->last_decstate_sz;
	solver_revert_planned_ops(solver, state->ops_stack_size);
	*frame = state->curr_frame;
	nframe = state->num_proc_frame;

	solver->num_proc_frame = nframe;
	solver->processing_stack.size = nframe * sizeof(*proc_frames);
	for (i = 0; i < nframe; i++)
		proc_frames[i] = state->proc_frames[i];

	frame->ipkg++;
	return 0;
}


/**
 * solver_add_deps_to_process() - Add new dependencies to process
 * @solver:     solver context to update
 * @frame:      pointer to the current processing frame
 * @deps:       compiled dependencies to process
 */
static
void solver_add_deps_to_process(struct solver* solver,
                                struct proc_frame* frame,
                                struct compiled_dep* deps)
{
	if (!deps)
		return;

	buffer_push(&solver->processing_stack, frame, sizeof(*frame));
	solver->num_proc_frame++;

	frame->dep = deps;
	frame->state = VALIDATION;
}


/**
 * solver_advance_processing() - update processing frame for next step
 * @solver:     solver context to update
 * @frame:      pointer to current processing frame. Updated on output
 *
 * Return: CONTINUE if there new iteration to run, DONE if the @solver
 * found a solution.
 */
static
int solver_advance_processing(struct solver* solver,
                              struct proc_frame* frame)
{
	struct buffer* proc_stack = &solver->processing_stack;

	do {
		if (frame->state == INSTALL_DEPS) {
			// Mark as installed the package whose dependency list has
			// just been processed
			solver_commit_pkg_install(solver,
			                          frame->dep->pkgname_id);
			frame->state = NEXT;
		}

		if (frame->state == NEXT) {
			frame->dep = compiled_dep_next(frame->dep);
			if (frame->dep) {
				frame->state = VALIDATION;
				break;
			}

			// Since the end of dependency list is reached, if the
			// processing stack is empty, then work is finished
			if (solver->num_proc_frame <= 0)
				return DONE;

			// Resume the previous processing frame from stack
			buffer_pop(proc_stack, frame, sizeof(*frame));
			solver->num_proc_frame--;
		}

	} while (frame->state == INSTALL_DEPS || frame->state == NEXT);

	return CONTINUE;
}


/**
 * solver_step_validation() - validate dependency against system state
 * @solver:     solver context to update
 * @frame:      pointer to the current processing frame
 *
 * The function checks if dependency @frame->dep being processed is not
 * already fullfiled by the system state or conflicts with it. It updates
 * @frame->state to point to the next processing step to perform.
 *
 * Return: 0 in case of success, -1 if backtracking is necessary.
 */
static
int solver_step_validation(struct solver* solver, struct proc_frame* frame)
{
	int id, is_staged;
	struct mmpkg* pkg;

	// Get package either installed or planned to be installed
	id = frame->dep->pkgname_id;
	pkg = solver->stage_lut[id];
	is_staged = 1;
	if (!pkg) {
		pkg = solver->inst_lut[id];
		is_staged = 0;
	}

	if (pkg) {
		// check package is suitable
		if (compiled_dep_pkg_match(frame->dep, pkg)) {
			frame->state = NEXT;
			return 0;
		}

		if (is_staged) {
			frame->state = BACKTRACK;
			return -1;
		}
	}

	frame->ipkg = 0;
	frame->state = SELECTION;
	return 0;
}


/**
 * solver_step_select_pkg() - select the package to install
 * @solver:     solver context to update
 * @frame:      pointer to the current processing frame
 *
 * The function select the package attempted to be installed to fulfill
 * @frame->dep.  It updates @frame->state to point to the next processing
 * step to perform.
 */
static
void solver_step_select_pkg(struct solver* solver, struct proc_frame* frame)
{
	struct mmpkg *pkg;
	int id = frame->dep->pkgname_id;

	// backup current state, ie before selected package is staged
	solver_save_decision_state(solver, frame);

	pkg = frame->dep->pkgs[frame->ipkg];
	solver_stage_pkg_install(solver, id, pkg);
	frame->state = INSTALL_DEPS;
}


/**
 * solver_step_install_deps() - perform dependency installation queing
 * @solver:     solver context to update
 * @frame:      pointer to the current processing frame
 *
 * The function queues for processing the dependencies of the package
 * staged for installation.
 * on.
 */
static
void solver_step_install_deps(struct solver* solver, struct proc_frame* frame)
{
	struct compiled_dep* deps;
	struct mmpkg* pkg;

	pkg = frame->dep->pkgs[frame->ipkg];

	deps = binindex_compile_pkgdeps(solver->binindex, pkg);
	solver_add_deps_to_process(solver, frame, deps);
}


/**
 * solver_solver_deps() - determine a solution given dependency list
 * @solver:     solver context to update
 * @initial_deps: initial dependency list to try to solve
 *
 * This function is the heart of the package dependency resolution. It
 * takes @initial_deps which is a list of compiled dependencies that
 * represent the constraints of the problem being solved and try to find
 * the actions (package install, removal, upgrade) that lead to those
 * requirements being met.
 *
 * Return: 0 if a solution has been found, -1 if no solution could be found
 */
static
int solver_solve_deps(struct solver* solver, struct compiled_dep* initial_deps)
{
	struct proc_frame frame = {.dep = initial_deps, .state = VALIDATION};

	mm_check(initial_deps != NULL);

	while (solver_advance_processing(solver, &frame) != DONE) {
		if (frame.state == BACKTRACK)
			if (solver_backtrack_on_decision(solver, &frame))
				return -1;

		if (frame.state == VALIDATION)
			if (solver_step_validation(solver, &frame))
				continue;

		if (frame.state == SELECTION)
			solver_step_select_pkg(solver, &frame);

		if (frame.state == INSTALL_DEPS)
			solver_step_install_deps(solver, &frame);
	};

	return 0;
}


/**
 * solver_create_action_stack() - Create a action stack from solution
 * @solver:     solver context to query
 *
 * Return: action stack corresponding to the solution previously found by
 * @solver.
 */
static
struct action_stack* solver_create_action_stack(struct solver* solver)
{
	struct action_stack* stk;
	struct planned_op* ops;
	struct mmpkg* pkg;
	int i, num_ops;

	stk = mmpack_action_stack_create();

	ops = solver->ops_stack.base;
	num_ops = solver->ops_stack.size / sizeof(*ops);

	for (i = 0; i < num_ops; i++) {
		pkg = ops[i].pkg;
		switch(ops[i].action) {
		case STAGE:
			// ignore
			break;

		case INSTALL:
			stk = mmpack_action_stack_push(stk, INSTALL_PKG, pkg);
			break;

		case REMOVE:
			stk = mmpack_action_stack_push(stk, REMOVE_PKG, pkg);
			break;

		default:
			mm_crash("Unexpected action type: %i", ops[i].action);
		}
	}

	return stk;
}


/**************************************************************************
 *                                                                        *
 *                         package installation requests                  *
 *                                                                        *
 **************************************************************************/

/**
 * request_to_compdep() - compile a dependency list from install request
 * @reqlist:    installation request list to convert
 * @binindex:   binary index to use to compiled the dependencies
 * @buff:       buffer to use to hold the compiled dependency list
 *
 * Return: pointer to the compiled dependency just created in case of
 * success. NULL if the package name in a request cannot be found.
 */
static
struct compiled_dep* compdeps_from_reqlist(const struct pkg_request* reqlist,
                                            const struct binindex* binindex,
                                            struct buffer* buff)
{
	STATIC_CONST_MMSTR(any_version, "any")
	struct mmpkg_dep dep = {0};
	struct compiled_dep* compdep = NULL;
	const struct pkg_request* req;

	mm_check(reqlist != NULL);

	for (req = reqlist; req != NULL; req = req->next) {
		dep.name = req->name;
		dep.min_version = req->version ? req->version : any_version;
		dep.max_version = dep.min_version;

		// Append to buff a new compiled dependency
		compdep = binindex_compile_dep(binindex, &dep, buff);
		if (!compdep) {
			error("Cannot find package: %s\n", req->name);
			return NULL;
		}
		if (compdep->num_pkg == 0) {
			error("Cannot find version %s of package %s\n",
			      req->version, req->name);
			return NULL;
		}
	}

	// mark last element as termination
	compdep->next_entry_delta = 0;
	return buff->base;
}


/**
 * mmpkg_get_install_list() -  parse package dependencies and return install order
 * @ctx:     the mmpack context
 * @reqlist:     requested package list to be installed
 *
 * In brief, this function will initialize a dependency ordered list with a
 * single element: the package passed as argument.
 *
 * Then while this list is not empty, take the last package of the list.
 * - if it introduces no new dependency, then stage it as an installed action
 *   in the action stack.
 * - if it has unmet *new* dependencies, append those to the dependency list,
 *   let the current dependency unmet in the list.
 *
 * This way, a dependency is removed from the list of needed package only if all
 * its dependency are already met.
 *
 * Returns: an action stack of the packages to be installed in the right order
 *          and at the correct version on success.
 *          NULL on error.
 */
LOCAL_SYMBOL
struct action_stack* mmpkg_get_install_list(struct mmpack_ctx * ctx,
                                            const struct pkg_request* reqlist)
{
	int rv;
	struct compiled_dep *deplist;
	struct solver solver;
	struct action_stack* stack = NULL;
	struct buffer deps_buffer;

	buffer_init(&deps_buffer);
	solver_init(&solver, ctx);

	// create initial dependency list from pkg_request list
	deplist = compdeps_from_reqlist(reqlist, &ctx->binindex, &deps_buffer);
	if (!deplist)
		goto exit;

	rv = solver_solve_deps(&solver, deplist);
	if (rv == 0)
		stack = solver_create_action_stack(&solver);

exit:
	solver_deinit(&solver);
	buffer_deinit(&deps_buffer);
	return stack;
}


/**************************************************************************
 *                                                                        *
 *                         package removal requests                       *
 *                                                                        *
 **************************************************************************/

/**
 * remove_package() - remove a package and its reverse dependencies
 * @pkgname:    name of package to remove
 * @binindex:   index of binary packages
 * @state:      temporary install state used to track removed packages
 * @stack:
 */
static
void remove_package(const mmstr* pkgname, struct binindex* binindex,
                    struct install_state* state, struct action_stack** stack)
{
	struct rdeps_iter iter;
	const struct mmpkg* rdep_pkg;
	const struct mmpkg* pkg;

	// Check this has not already been done
	pkg = install_state_get_pkg(state, pkgname);
	if (!pkg)
		return;

	// Mark it now remove from state (this avoid infinite loop in the
	// case of circular dependency)
	install_state_rm_pkgname(state, pkgname);

	// First remove recursively the reverse dependencies
	rdep_pkg = rdeps_iter_first(&iter, pkg, binindex, state);
	while (rdep_pkg) {
		remove_package(rdep_pkg->name, binindex, state, stack);
		rdep_pkg = rdeps_iter_next(&iter);
	}

	*stack = mmpack_action_stack_push(*stack, REMOVE_PKG, pkg);
}


/**
 * mmpkg_get_remove_list() -  compute a remove order
 * @ctx:     the mmpack context
 * @reqlist: requested package list to be removed
 *
 * Returns: an action stack of the actions to be applied in order to remove
 * the list of package in @reqlist in the right order.
 */
LOCAL_SYMBOL
struct action_stack* mmpkg_get_remove_list(struct mmpack_ctx * ctx,
                                           const struct pkg_request* reqlist)
{
	struct install_state state;
	struct action_stack * actions = NULL;
	const struct pkg_request* req;

	actions = mmpack_action_stack_create();

	// Copy the current install state of prefix context in order to
	// simulate the operation done on installed package list
	install_state_copy(&state, &ctx->installed);

	for (req = reqlist; req; req = req->next)
		remove_package(req->name, &ctx->binindex, &state, &actions);

	return actions;
}


LOCAL_SYMBOL
void mmpack_action_stack_dump(struct action_stack * stack)
{
	int i;

	for (i =  0 ; i < stack->index ; i++) {
		if (stack->actions[i].action == INSTALL_PKG)
			printf("INSTALL: ");
		else if (stack->actions[i].action == REMOVE_PKG)
			printf("REMOVE: ");

		mmpkg_dump(stack->actions[i].pkg);
	}
}


LOCAL_SYMBOL
int confirm_action_stack_if_needed(int nreq, struct action_stack const * stack)
{
	int i, rv;

	if (stack->index == 0) {
		printf("Nothing to do.\n");
		return 0;
	}

	printf("Transaction summary:\n");

	for (i =  0 ; i < stack->index ; i++) {
		if (stack->actions[i].action == INSTALL_PKG)
			printf("INSTALL: ");
		else if (stack->actions[i].action == REMOVE_PKG)
			printf("REMOVE: ");

		printf("%s (%s)\n", stack->actions[i].pkg->name,
		                  stack->actions[i].pkg->version);
	}

	if (nreq == stack->index) {
		/* mmpack is installing as many packages as requested:
		 * - they are exactly the one requested
		 * - they introduce no additional dependencies
		 *
		 * proceed with install without confirmation */
		return 0;
	}

	rv = prompt_user_confirm();
	if (rv != 0)
		printf("Abort.\n");

	return rv;
}
