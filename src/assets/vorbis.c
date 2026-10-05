/* stb_vorbis, built on its own (outside the engine's strict warnings). Sounds and music are
   decoded from memory, so the stdio and push-data parts are left out. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
