all: clean deb apk

deb: deb-clean deb-buster

deb-stretch: deb-clean
	CURRENT_UID="$(shell id -u):$(shell id -g)" docker-compose run --rm debian-stretch-build;

deb-buster: deb-clean
	CURRENT_UID="$(shell id -u):$(shell id -g)" docker-compose run --rm debian-buster-build;

apk: apk-clean
	docker-compose run --rm alpine-build;

deb-clean:
	@rm -rf ./debian

apk-clean:
	@rm -rf ./alpine

clean:
	@rm -rf ./debian
	@rm -rf ./alpine
