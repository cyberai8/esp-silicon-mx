# IDF 6 moved cJSON out of the built-in `json` component.
# Keep a `json` alias so dependents (e.g. 78/esp-wifi-connect) that
# `REQUIRES json` still resolve to espressif/cjson headers + libs.
idf_component_register(
    REQUIRES espressif__cjson
)
