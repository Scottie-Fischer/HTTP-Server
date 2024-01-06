#-----------------------------------#
#
#
#
#
#-----------------------------------#

output: httpserver.c httpserver.o
	gcc httpserver.o -lm -o httpserver -lpthread

httpserver.o: httpserver.c
	gcc -c -Wall -Wextra -Wpedantic -Wshadow httpserver.c -lpthread -lm

httpserver: httpserver.c
	gcc -c -Wall -g -Wextra -Wpedantic -Wshadow httpserver.c -lpthread -lm

spotless:
	rm *.o

clean:
	rm *.o httpserver
