all:
	gcc -std=gnu11 src/*.c tests/*.c -lmariadb -luv -I"/usr/local/include/mariadb/" -L"/usr/local/lib/mariadb" -I"./include" -g -o async