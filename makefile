all: httpd

httpd: httpd.c
	gcc -W -Wall -g -o httpd httpd.c -lpthread

clean:
	rm httpd
