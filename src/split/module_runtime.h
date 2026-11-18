#ifndef SPLIT_MODULE_RUNTIME_H
#define SPLIT_MODULE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"
#include "split/ipc.h"

/* Called by an os_RunPrgm callback right before it re-enters main().
 * TI-OS only reaches that callback after the child program returned, so the
 * flag proves "this launch is the resumed parent", independent of whatever
 * return_to value a deeper forwarder wrote into CE_REQ. */
void ce_module_runtime_mark_reentry(void);
bool ce_module_runtime_was_reentered(void);
/* Share the bounded pending-name copy with the forwarding path. */
void ce_runtime_set_pending_module(ce_shared_state_t *state, const char *program);
#ifdef DESKTOP_BUILD
void ce_module_runtime_reset_reentry(void); /* test hook */
#endif

bool ce_module_runtime_load(ce_request_t *req,
                            ce_response_t *resp,
                            ce_shared_state_t *state,
                            ast_node_t **root_out,
                            bool *had_equals_out);

bool ce_module_runtime_resume_forward(const char *self_program,
                                      ce_request_t *req,
                                      ce_shared_state_t *state);

/* Like ce_module_runtime_resume_forward(), but does not hand the response
 * up: returns true when this launch is the resumed parent and the child's
 * CE_RESP is readable, filling *resp and the child's program name. Lets a
 * multi-strategy forwarder decide on hardware whether to try the next child
 * (os_RunPrgm never returns there, so an in-process loop only reaches its
 * first child). CE_STATE is left untouched. */
bool ce_module_runtime_resume_peek(const char *self_program,
                                   ce_request_t *req,
                                   ce_shared_state_t *state,
                                   ce_response_t *resp,
                                   char *child_out,
                                   size_t child_out_size);

/* Clears the pending child/response so this program may forward again. */
void ce_module_runtime_reclaim_forward(const char *self_program,
                                       ce_shared_state_t *state);

bool ce_module_runtime_recover_child_failure(const char *self_program,
                                             const char *pending_target,
                                             ce_request_t *req,
                                             ce_shared_state_t *state,
                                             char *err_buf,
                                             size_t err_buf_size);

void ce_module_runtime_finish(const ce_request_t *req,
                              const ce_response_t *resp,
                              ce_shared_state_t *state);

bool ce_module_runtime_forward(const char *caller,
                               const char *program,
                               ce_request_t *req,
                               ce_shared_state_t *state,
                               ce_response_t *resp,
                               char *err_buf,
                               size_t err_buf_size);

#endif /* SPLIT_MODULE_RUNTIME_H */
