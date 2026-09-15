#include "audio_pipeline.h"

#include <inttypes.h>
#include <stdio.h>

#include "recording_store.h"

uint32_t audio_pipeline_session_id(void)
{
    const char *path = recording_store_current_path();
    if (!path || !path[0]) {
        return 0;
    }

    uint32_t session_id = 0;
    if (sscanf(path, "/recordings/%" SCNu32 "-", &session_id) != 1) {
        return 0;
    }
    return session_id;
}
