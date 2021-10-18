# Copied from
#  https://github.com/docker-library/python/blob/master/3.9/bullseye/Dockerfile
# and modified.

FROM nvidia/cuda:11.0-devel-ubuntu20.04

ARG commit

# ensure local python is preferred over distribution python
ENV PATH /usr/local/bin:$PATH

# http://bugs.python.org/issue19846
# > At the moment, setting "LANG=C" on a Linux system *fundamentally breaks Python 3*, and that's not OK.
ENV LANG C.UTF-8

# Don't ask questions during build.
ENV DEBIAN_FRONTEND=noninteractive

# Dependencies.
RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential \
	bzip2 \
	libbluetooth-dev \
	libbz2-dev \
	libcurl4-openssl-dev \
	libdb-dev \
	libffi-dev \
	libgdbm-dev \
	liblzma-dev \
	libncurses5-dev \
	libncursesw5-dev \
	libpng-dev \
	libreadline-dev \
	libsqlite3-dev \
	libssl-dev \
	tk-dev \
	uuid-dev \
	wget \
	&& rm -rf /var/lib/apt/lists/*

RUN set -ex \
	\
	&& wget --no-verbose -O python.tar.gz "https://github.com/colesbury/nogil/tarball/$commit" \
	&& mkdir -p /usr/src/python \
	&& tar -xC /usr/src/python --strip-components=1 -f python.tar.gz \
	&& rm python.tar.gz \
	\
	&& cd /usr/src/python \
	&& gnuArch="$(dpkg-architecture --query DEB_BUILD_GNU_TYPE)" \
	&& ./configure \
		--build="$gnuArch" \
		--enable-loadable-sqlite-extensions \
		--enable-optimizations \
		--enable-option-checking=fatal \
		--enable-shared \
		--with-system-expat \
		--with-system-ffi \
	&& make -j "$(nproc)" \
	&& make install \
	&& rm -rf /usr/src/python \
	\
	&& find /usr/local -depth \
		\( \
			\( -type d -a \( -name test -o -name tests -o -name idle_test \) \) \
			-o \( -type f -a \( -name '*.pyc' -o -name '*.pyo' -o -name '*.a' \) \) \
		\) -exec rm -rf '{}' + \
	\
	&& ldconfig \
	\
	&& python3 --version

# make some useful symlinks that are expected to exist
RUN cd /usr/local/bin \
	&& ln -s idle3 idle \
	&& ln -s pydoc3 pydoc \
	&& ln -s python3 python \
	&& ln -s python3-config python-config \
	&& ln -s pip3 pip

CMD ["python3"]


# Docker commands:
#   docker rm nogil -v
#   docker build -t nogil .
#   docker run --gpus all --rm --name nogil nogil
# or
#   docker run --gpus all -it --entrypoint /bin/bash nogil
