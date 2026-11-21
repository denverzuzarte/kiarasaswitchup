main : main.o 

main.o : main.c
	g++

## Make the 1M-event dev set with the published generator (needs `generate` on PATH)
dev : generate --seed 1 --events 1000000 --clicks 250000 --output $(DEV)
