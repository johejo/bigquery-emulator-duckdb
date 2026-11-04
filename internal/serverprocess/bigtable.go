package serverprocess

import (
	"context"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"syscall"

	adminpb "cloud.google.com/go/bigtable/admin/apiv2/adminpb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// Bigtable owns an isolated nixpkgs cbtemulator process.
type Bigtable struct {
	Host    string
	process *process
}

func StartBigtable(ctx context.Context) (*Bigtable, error) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	host := listener.Addr().String()
	if err := listener.Close(); err != nil {
		return nil, err
	}
	cmd := exec.Command("cbtemulator", "-address", host)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Stderr = os.Stderr
	p := &Bigtable{Host: host}
	p.process, err = startProcess(cmd, nil)
	if err != nil {
		return nil, err
	}
	conn, err := grpc.NewClient(host, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, errors.Join(err, p.Stop())
	}
	defer conn.Close()
	client := adminpb.NewBigtableTableAdminClient(conn)
	if err := p.process.poll(ctx, func(ctx context.Context) bool {
		_, err := client.ListTables(ctx, &adminpb.ListTablesRequest{Parent: "projects/test/instances/test"})
		return err == nil
	}); err != nil {
		return nil, errors.Join(fmt.Errorf("waiting for cbtemulator: %w", err), p.Stop())
	}
	if _, err := client.CreateTable(ctx, &adminpb.CreateTableRequest{
		Parent: "projects/test/instances/test", TableId: "probe",
		Table: &adminpb.Table{ColumnFamilies: map[string]*adminpb.ColumnFamily{"family": {}}},
	}); err != nil {
		return nil, errors.Join(err, p.Stop())
	}
	return p, nil
}

func (p *Bigtable) Stop() error { return p.process.stop(syscall.SIGTERM, true, 0) }
