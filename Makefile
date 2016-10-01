all:
	gcc -std=gnu11 *.c -lmariadb -luv -I"/usr/local/include/mariadb/" -L"/usr/local/lib/mariadb" -g -o async