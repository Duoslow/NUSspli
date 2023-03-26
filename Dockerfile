FROM ghcr.io/wiiu-env/devkitppc:20230326

ENV DEBIAN_FRONTEND=noninteractive \
 PATH=$DEVKITPPC/bin:$PATH \
 WUT_ROOT=$DEVKITPRO/wut

WORKDIR /

COPY --from=ghcr.io/wiiu-env/libmocha:20220919 /artifacts $DEVKITPRO
COPY --from=ghcr.io/wiiu-env/librpxloader:20220903 /artifacts $DEVKITPRO

# Install apt-fast
RUN mkdir -p /usr/share/man/man1 /usr/share/man/man2 && \
 /bin/bash -c "$(curl -sL https://raw.githubusercontent.com/V10lator/NUSspli/master/apt-fast/install.sh)"

# Upgrade the systen
RUN apt-fast -y --no-install-recommends update && \
 apt-fast -y --no-install-recommends upgrade

# Install the requirements to package the homebrew
RUN apt-fast -y install --no-install-recommends autoconf automake libtool openjdk-11-jre-headless && \
 apt-fast clean

# Install the requirements to build the homebrew
RUN git clone --recursive https://github.com/yawut/libromfs-wiiu --single-branch && \
 cd libromfs-wiiu && \
 make -j$(nproc) && \
 make install

RUN git config --global --add safe.directory /project

WORKDIR /project
