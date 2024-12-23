###################################################################
 #		   Copyright (c) 2016 by TAOS Technologies, Inc.
 #				     All rights reserved.
 #
 #  This file is proprietary and confidential to TAOS Technologies.
 #  No part of this file may be reproduced, stored, transmitted, 
 #  disclosed or used in any form or by any means other than as 
 #  expressly provided by the written permission from Jianhui Tao
 #
###################################################################
from util.cases import *
from util.sql import *

class TDTestCase:
    def init(self, conn, logSql, replicaVar=1):
        tdLog.debug(f"start to init {__file__}")
        self.replicaVar = int(replicaVar)
        tdSql.init(conn.cursor(), logSql)

    def run(self):
        tdSql.execute('CREATE DATABASE db vgroups 1 replica 2;')

    def stop(self):
        tdSql.close()
        tdLog.success(f"{__file__} successfully executed")
	
tdCases.addLinux(__file__, TDTestCase())
tdCases.addWindows(__file__, TDTestCase())