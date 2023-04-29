app:
	GOOS=linux GOARCH=arm GOARM=5 go build .

deploy: app
	scp -O exporter root@10.8.30.179:/mnt/sd/bin/
