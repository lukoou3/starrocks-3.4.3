package com.starrocks.analysis;


import com.starrocks.common.Config;
import com.starrocks.common.FeConstants;
import com.starrocks.common.Pair;
import com.starrocks.qe.ConnectContext;
import com.starrocks.sql.StatementPlanner;
import com.starrocks.sql.ast.StatementBase;
import com.starrocks.sql.parser.SqlParser;
import com.starrocks.sql.plan.ExecPlan;
import com.starrocks.utframe.StarRocksAssert;
import com.starrocks.utframe.UtFrameUtils;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

public class SqlAnalyzeTest {
    private static StarRocksAssert starRocksAssert;

    @BeforeAll
    public static void setUp() throws Exception {
        UtFrameUtils.createMinStarRocksCluster();
        Config.show_execution_groups = false;
        FeConstants.showFragmentCost = false;
        FeConstants.setLengthForVarchar = false;
        String createTblStmtStr = "create table db1.tbl1(k1 varchar(32), k2 varchar(32), k3 varchar(32), k4 int) "
                + "AGGREGATE KEY(k1, k2,k3,k4) distributed by hash(k1) buckets 3 properties('replication_num' = '1');";
        String createTable1 = "create table if not exists test_tbl(\n" +
                "    cate string,\n" +
                "    timestamp datetime not null,\n" +
                "    cnt bigint,\n" +
                "    content string,\n" +
                "    txt string\n" +
                ")\n" +
                "duplicate key(cate, timestamp)\n" +
                "partition by date_trunc('day', timestamp)\n" +
                "distributed by random\n" +
                "properties (\n" +
                "    \"bucket_size\" = \"104857600\",\n" +
                "    \"mutable_bucket_num\" = \"2\",\n" +
                "    \"replication_num\" = \"1\",\n" +
                "    \"partition_live_number\" = \"60\"\n" +
                ");";


        starRocksAssert = new StarRocksAssert();
        starRocksAssert.withDatabase("db1").useDatabase("db1");
        starRocksAssert.withTable(createTblStmtStr)
                .withTable(createTable1);
        FeConstants.enablePruneEmptyOutputScan = false;
    }

    @Test
    void testQuery() throws Exception {
        String sql = "select 1 + 2 a, if(false,1,2) b, time_slice('2025-01-18 15:44:31', INTERVAL 5 second) c, " +
                "substr('2025-01-18 15:44:31', 1, 10) d";
        Pair<String, ExecPlan> planAndFragment = UtFrameUtils.getPlanAndFragment(starRocksAssert.getCtx(), sql);
        System.out.println(planAndFragment);
    }

    @Test
    void testParseAndPlanQuery() throws Exception {
        String sql = "select 1 + 2 a, if(false,1,2) b, time_slice('2025-01-18 15:44:31', INTERVAL 5 second) c, " +
                "substr('2025-01-18 15:44:31', 1, 10) d";
        ConnectContext ctx = starRocksAssert.getCtx();
        // 执行完parse得到的stmt，其中的函数还没有解析到具体的函数
        StatementBase stmt = SqlParser.parse(sql, ctx.getSessionVariable()).get(0);
        // 执行完plan，会原地修改stmt，其中的函数会被解析设置
        ExecPlan execPlan = new StatementPlanner().plan(stmt, ctx);
        System.out.println(stmt);
        System.out.println(execPlan);
    }
}
