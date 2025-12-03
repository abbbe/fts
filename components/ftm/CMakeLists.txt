idf_component_register(
    SRCS "ftm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash clock crm
)
