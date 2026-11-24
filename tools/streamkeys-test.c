#include "src/system/core/config.h"
#include "src/system/streaming/streamkeys.h"

#include <assert.h>
#include <string.h>

int main(int argc, char **argv) {
	assert(argc == 4);
	if (strcmp(argv[1], "none") == 0) {
		streamkeys_init();
		assert(!streamkeys_source() && !streamkeys_tidal_client_id() && !streamkeys_tidal_client_secret());
		return 0;
	}
	if (strcmp(argv[1], "override") == 0)
		config_set("streaming", "keys_file", argv[2]);
	streamkeys_init();
	assert(streamkeys_source() && strcmp(streamkeys_source(), argv[2]) == 0);
	assert(streamkeys_tidal_client_id() && strcmp(streamkeys_tidal_client_id(), argv[3]) == 0);
	assert(streamkeys_tidal_client_secret() && strcmp(streamkeys_tidal_client_secret(), "fixture-secret") == 0);
}
