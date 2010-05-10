#include "plugin.h"
#include <mad.h>

static const struct PLUGIN plugin_info = {
	"libmad MPEG audio decoder"
};

const struct PLUGIN *get_info()
{
	return &plugin_info;
}
