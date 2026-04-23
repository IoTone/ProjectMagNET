idf_component_register(
    SRCS "craw_hive_proto.c"
         "craw_hive_node.c"
         "craw_hive_ruler.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer esp_netif esp_event lwip json mbedtls mdns
)
