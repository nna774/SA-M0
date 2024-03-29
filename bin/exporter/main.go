package main

import (
	"fmt"
	"os"
	"time"

	"github.com/nna774/sa-m0/bp35a1"
)

func main() {
	client, err := bp35a1.NewClient("/dev/ttymxc2")
	if err != nil {
		panic(err)
	}
	info, err := client.SKInfo()
	if err != nil {
		panic(err)
	}
	fmt.Printf("info: %+v\n", info)

	id := os.Getenv("ID")
	pass := os.Getenv("PASS")
	if id == "" || pass == "" {
		panic("need id/pass")
	}

	err = client.Auth(id, pass)
	if err != nil {
		panic(err)
	}
	fmt.Println("after auth")

	scan, err := client.Scan()
	if err != nil {
		panic(err)
	}

	fmt.Printf("scan: %+v\n", scan)
	err = client.SetChannel(scan.Channel, scan.PanID)
	if err != nil {
		panic(err)
	}

	addr, err := client.SKLL64(scan.Addr)
	if err != nil {
		panic(err)
	}
	fmt.Printf("addr: %+v\n", addr)

	err = client.SKJOIN(addr)
	if err != nil {
		panic(err)
	}

	for {
		val, err := client.ReadInstantaneousPower(addr)
		if err != nil {
			panic(err)
		}
		fmt.Printf("val: %+v\n", val)
		time.Sleep(1 * time.Second)
	}
}
