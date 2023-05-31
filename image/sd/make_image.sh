#! /bin/sh -xe

export CURL='curl -k' # note: this is insecure!

mkdir -p etc
mkdir -p bin

[ -e bin/dropbear ] || $CURL https://storage.googleapis.com/nona7-data/tmp/dropbear -o bin/dropbear && chmod +x bin/dropbear
[ -e bin/dropbearkey ] || $CURL https://storage.googleapis.com/nona7-data/tmp/dropbearkey -o bin/dropbearkey && chmod +x bin/dropbearkey
[ -e bin/scp] || $CURL https://storage.googleapis.com/nona7-data/tmp/scp -o bin/scp && chmod +x bin/scp

[ -e etc/host.key ] || ./bin/dropbearkey -t ed25519 -f etc/host.key

# TODO: https://github.com/nna774/ud-co2s-exporter
