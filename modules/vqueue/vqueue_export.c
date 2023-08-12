#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <re.h>
#include <baresip.h>
#include "vqueue.h"

extern struct ausrc *ausrc;
extern struct auplay *auplay;

int module_init(void)
{
	int err = auplay_register(&auplay, baresip_auplayl(),
				"vqueue", vqueue_play_alloc);
	err |= ausrc_register(&ausrc, baresip_ausrcl(),
				"vqueue", vqueue_src_alloc);

	if (err) {
		return err;
	}
}

int module_close(void)
{
	auplay = (struct auplay*)mem_deref((void*)auplay);
	ausrc  = (struct ausrc*)mem_deref((void*)ausrc);

	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(vqueue) = {
	"vqueue_enqueue",
	"vqueue_stop"
	"vqueue_cancel",
	module_init,
	module_close
};