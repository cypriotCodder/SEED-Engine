// stb_image's implementation, limited to the formats the editor imports and to decoding from memory.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "stb_image.h"
