idf_component_register(
  SRCS "ld2450.c" "ld2450_parser.c" "ld2450_zone.c" "ld2450_zone_csv.c" "ld2450_cmd.c"
  INCLUDE_DIRS "include"
  REQUIRES driver freertos esp_timer log
)

