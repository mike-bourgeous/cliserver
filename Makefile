all:
	gcc -s -O2 -Wall cliserver.c -o cliserver -levent
debug:
	gcc -g -Wall cliserver.c -o cliserver -levent
clean:
	rm -f cliserver
