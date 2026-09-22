# The IDF build takes only the platform-independent core plus the IDF backend.
# main.cpp and port_arduino.cpp are the Arduino side and are deliberately absent.
idf_component_register(
  SRCS
    "forth.cpp"
    "forth_ffi.cpp"
    "forth_files.cpp"
    "port_idf.c"
    "main_idf.cpp"
    "ble_hid_idf.c"
  INCLUDE_DIRS "."
  REQUIRES
    esp_timer
    esp_driver_gpio
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_hid
    bt
    nvs_flash
    joltwallet__littlefs
)
