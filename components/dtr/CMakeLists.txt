idf_component_register(
    SRCS "dtr.c"
    INCLUDE_DIRS "include"
    REQUIRES driver dtc
    PRIV_REQUIRES esp_wifi clock
)
