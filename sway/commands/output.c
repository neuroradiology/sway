#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/output.h"
#include "list.h"
#include "log.h"

// must be in order for the bsearch
static struct cmd_handler output_handlers[] = {
	{ "background", output_cmd_background },
	{ "bg", output_cmd_background },
	{ "disable", output_cmd_disable },
	{ "dpms", output_cmd_dpms },
	{ "enable", output_cmd_enable },
	{ "mode", output_cmd_mode },
	{ "pos", output_cmd_position },
	{ "position", output_cmd_position },
	{ "res", output_cmd_mode },
	{ "resolution", output_cmd_mode },
	{ "scale", output_cmd_scale },
	{ "transform", output_cmd_transform },
};

struct cmd_results *cmd_output(int argc, char **argv) {
	struct cmd_results *error = checkarg(argc, "output", EXPECTED_AT_LEAST, 1);
	if (error != NULL) {
		return error;
	}

	// The NOOP-1 output is a dummy output used when there's no outputs
	// connected. It should never be configured.
	if (strcasecmp(argv[0], root->noop_output->wlr_output->name) == 0) {
		return cmd_results_new(CMD_FAILURE,
				"Refusing to configure the no op output");
	}

	struct output_config *output = new_output_config(argv[0]);
	if (!output) {
		sway_log(SWAY_ERROR, "Failed to allocate output config");
		return NULL;
	}
	argc--; argv++;

	config->handler_context.output_config = output;

	while (argc > 0) {
		config->handler_context.leftovers.argc = 0;
		config->handler_context.leftovers.argv = NULL;

		if (find_handler(*argv, output_handlers, sizeof(output_handlers))) {
			error = config_subcommand(argv, argc, output_handlers,
					sizeof(output_handlers));
		} else {
			error = cmd_results_new(CMD_INVALID,
				"Invalid output subcommand: %s.", *argv);
		}

		if (error != NULL) {
			goto fail;
		}

		argc = config->handler_context.leftovers.argc;
		argv = config->handler_context.leftovers.argv;
	}

	config->handler_context.output_config = NULL;
	config->handler_context.leftovers.argc = 0;
	config->handler_context.leftovers.argv = NULL;

	output = store_output_config(output);

	// If reloading, the output configs will be applied after reading the
	// entire config and before the deferred commands so that an auto generated
	// workspace name is not given to re-enabled outputs.
	if (!config->reloading) {
		apply_output_config_to_outputs(output);
	}

	return cmd_results_new(CMD_SUCCESS, NULL);

fail:
	config->handler_context.output_config = NULL;
	free_output_config(output);
	return error;
}
