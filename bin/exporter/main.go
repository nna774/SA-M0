package main

import (
	"fmt"

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
}
