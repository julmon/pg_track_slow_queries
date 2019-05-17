all: clean deb apk

deb: deb-clean
	docker-compose run --rm debian-build;

apk: apk-clean
	docker-compose run --rm alpine-build;

deb-clean:
	@rm -rf ./debian

apk-clean:
	@rm -rf ./alpine

clean:
	@rm -rf ./debian
	@rm -rf ./alpine
