package cmd

import (
	"context"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/taosdata/taoskeeper/db"
	"github.com/taosdata/taoskeeper/infrastructure/config"
	"github.com/taosdata/taoskeeper/util"
)

func TestTransferTaosdClusterBasicInfo(t *testing.T) {
	config.InitConfig()

	conn, _ := db.NewConnector("root", "taosdata", "127.0.0.1", 6041, false)
	defer conn.Close()

	dbName := "db_202412031539"
	conn.Exec(context.Background(), "create database "+dbName, util.GetQidOwn())
	defer conn.Exec(context.Background(), "drop database "+dbName, util.GetQidOwn())

	conn, _ = db.NewConnectorWithDb("root", "taosdata", "127.0.0.1", 6041, dbName, false)
	defer conn.Close()

	cmd := Command{conn: conn, fromTime: time.Now().Add(time.Duration(1 * time.Hour))}
	cmd.TransferTaosdClusterBasicInfo()

	testCases := []struct {
		ep      string
		wantErr bool
	}{
		{"", false},
		{"hello", false},
		{strings.Repeat("a", 128), false},
		{strings.Repeat("a", 255), false},
		{strings.Repeat("a", 256), true},
	}

	conn.Exec(context.Background(),
		"create table d0 using taosd_cluster_basic tags('cluster_id')", util.GetQidOwn())

	for _, tc := range testCases {
		sql := fmt.Sprintf("insert into d0 (ts, first_ep) values(%d, '%s')", time.Now().UnixMilli(), tc.ep)
		_, err := conn.Exec(context.Background(), sql, util.GetQidOwn())
		if tc.wantErr {
			assert.Error(t, err) // [0x2653] Value too long for column/tag: endpoint
		} else {
			assert.NoError(t, err)
		}
	}
}
