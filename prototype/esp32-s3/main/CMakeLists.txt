idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)

# V8.4 audio stored in flash (not PSRAM)
target_add_binary_data(${COMPONENT_LIB} "music_16k_mono.pcm" BINARY)
