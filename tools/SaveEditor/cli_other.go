//go:build !windows

package main

import (
	"bytes"
	"fmt"
	"os"
)

func main() {
	if os.Args[1] == "rr3" {
		raw, _ := os.ReadFile(os.Args[2])
		sv, err := ParseRR3(os.Args[2], raw)
		if err != nil {
			fmt.Println("ERR", err)
			return
		}
		fmt.Println("root", len(sv.Root), "identical rebuild:", bytes.Equal(sv.Build(), raw))
		return
	}
	sv, err := LoadNFS(os.Args[2])
	if err != nil {
		fmt.Println("ERR", err)
		return
	}
	fmt.Println("files", len(sv.Files), "leaves", len(sv.Leaves))
	for _, m := range nfsMain {
		if l := sv.ByPath[m.Path]; l != nil {
			fmt.Println(m.Label, "=", l.Get())
		} else {
			fmt.Println(m.Label, "MISSING")
		}
	}
	if len(os.Args) > 3 {
		fmt.Println(sv.SetMain(nfsMain[0], "5000000"), sv.SetMain(nfsMain[1], "99999"), sv.SetMain(nfsMain[2], "20"))
		b, err := sv.Save("test")
		fmt.Println("saved", b, err)
		s2, err := LoadNFS(os.Args[2])
		if err != nil {
			fmt.Println("RELOAD ERR", err)
			return
		}
		for _, m := range nfsMain[:3] {
			fmt.Println(m.Label, s2.ByPath[m.Path].Get(), s2.ByPath[m.Also] != nil)
		}
		fmt.Println(s2.ByPath["Currencies/ProgressionModel/_CurrencyInventory/_Currencies[Cash]/_BalanceEarned/Value"].Get())
	}
}
