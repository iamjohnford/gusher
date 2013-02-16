gusher:		main.o
		gcc -o gusher main.o -lmicrohttpd -lguile

main.o:		main.c
		gcc -c -Wall main.c

clean:
		rm *.o gusher
