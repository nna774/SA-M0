package bp35a1

import (
	"bufio"
	"fmt"
	"time"

	"github.com/tarm/serial"
)

type Client interface {
	SKInfo() (*Info, error)
}

func NewClient(path string) (Client, error) {
	c := &serial.Config{
		Name:        path,
		Baud:        115200,
		ReadTimeout: 5 * time.Second,
		Size:        8,
		Parity:      serial.ParityNone,
	}
	s, err := serial.OpenPort(c)
	if err != nil {
		return nil, err
	}

	return &client{
		port: s,
	}, nil
}

type Info struct {
	Value string
}

type client struct {
	port *serial.Port
}

func (c *client) SKInfo() (*Info, error) {
	_, err := c.port.Write([]byte("SKINFO\r\n"))
	if err != nil {
		return nil, err
	}
	s := bufio.NewScanner(c.port)
	s.Scan()
	buf := s.Text()
	if buf != "SKINFO" {
		return nil, fmt.Errorf("got strange response: %v", buf)
	}

	s.Scan()
	buf = s.Text()
	if len(buf) < 4 || buf[0:4] == "FAIL" {
		return nil, fmt.Errorf("got fail: %v", buf)
	}
	value := buf

	s.Scan()
	buf = s.Text()

	if buf != "OK" {
		return nil, fmt.Errorf("not OK: %v, %v", value, buf)
	}
	return &Info{Value: value}, nil
}

// See https://golang.org/doc/effective_go.html#blank_implements
var _ Client = (*client)(nil)
