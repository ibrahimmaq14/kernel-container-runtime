all:
	$(MAKE) -C boilerplate all

ci:
	$(MAKE) -C boilerplate ci

clean:
	$(MAKE) -C boilerplate clean

.PHONY: all ci clean
