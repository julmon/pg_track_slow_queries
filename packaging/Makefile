all: clean deb

deb: deb-clean
	docker-compose run --rm debian-build;

deb-clean:
	@rm -rf ./debian

clean:
	@rm -rf ./debian
