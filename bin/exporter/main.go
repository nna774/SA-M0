package main

import (
	"fmt"
	"os"

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
	fmt.Printf("info: %v\n", info)

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
	fmt.Printf("scan: %v\n", scan)
}
