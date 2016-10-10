set(EXTERNAL_LIBS "mariadb uv")

add_executable(test1 test1.c)
target_link_libraries(test1 mysqlasync)
target_link_libraries(test1 mariadb)
target_link_libraries(test1 uv)

install(TARGETS test1 DESTINATION bin/libmysqlasync)
