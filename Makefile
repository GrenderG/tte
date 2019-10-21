tte: tte.c
	$(CC) tte.c -o tte -Wall -Wextra -pedantic -std=c99 -g

tte_release: tte.c
	$(CC) tte.c -o tte -std=c99

install: tte_release
	sudo cp tte /usr/local/bin/
	sudo chmod +x /usr/local/bin/
