idf_component_register(
    SRCS "snake_matrix_main.c" "qmi8658_esp.c" "glcdfont.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver
)
