#include "audio_pipeline.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "recording_store.h"

uint32_t audio_pipeline_session_id(void)
{
    const char *path = recording_store_current_path();
    if (!path || !path[0]) {
        return 0;
    }

    /* The directory depends on where recording_store mounted (SD card or
     * internal flash), so parse the "<session>-<suffix>" file name only. */
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    uint32_t session_id = 0;
    if (sscanf(name, "%" SCNu32 "-", &session_id) != 1) {
        return 0;
    }
    return session_id;
}
