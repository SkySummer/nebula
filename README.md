# Nebula

## 部署流程

以下步骤以 Linux 环境为前提，命令默认在仓库根目录执行。

### 1. 准备依赖

部署前需要先安装以下工具和库：

- `gcc 13+` 或兼容的 C++23 编译器
- `cmake`
- `pkg-config`
- OpenSSL 开发库
- `libpqxx` 开发库
- `podman` 或 `docker`

本文的容器命令默认以 `podman` 为例；如果你使用 `docker`，可将命令中的 `podman` 直接替换为 `docker`。

### 2. 构建服务

```bash
cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release
cmake --build server/build -j"$(nproc)"
```

构建完成后，服务可执行文件位于：

```bash
./server/build/bin/nebula
```

### 3. 运行测试

如果你希望在部署前先做一次基础验证，可以直接运行：

```bash
ctest --test-dir server/build --output-on-failure
```

说明：

- 这条命令会执行全部已注册测试。
- 数据库相关测试依赖 `NEBULA_TEST_DATABASE_*` 环境变量。
- 如果这些环境变量未配置，数据库相关测试会按约定跳过，不会作为失败处理。

如果你希望连同数据库相关测试一起完整执行，建议准备一个独立测试库，不要复用部署库。

启动测试数据库：

```bash
podman run -d \
    --name nebula-postgres-test \
    -e POSTGRES_DB=nebula_test \
    -e POSTGRES_USER=nebula \
    -e POSTGRES_PASSWORD=nebula \
    -p 15432:5432 \
    -v nebula-pgdata-test:/var/lib/postgresql/data \
    docker.io/library/postgres:16
```

等待数据库就绪：

```bash
until podman exec nebula-postgres-test pg_isready -U nebula -d nebula_test >/dev/null 2>&1; do
    sleep 1
done
```

初始化测试库：

```bash
podman exec -i nebula-postgres-test \
    psql -U nebula -d nebula_test \
    < server/deploy/postgres/init_schema.sql
```

设置测试环境变量：

```bash
export NEBULA_TEST_DATABASE_HOST=127.0.0.1
export NEBULA_TEST_DATABASE_PORT=15432
export NEBULA_TEST_DATABASE_NAME=nebula_test
export NEBULA_TEST_DATABASE_USER=nebula
export NEBULA_TEST_DATABASE_PASSWORD=nebula
```

然后重新执行：

```bash
ctest --test-dir server/build --output-on-failure
```

注意：数据库相关测试会执行 `TRUNCATE TABLE users RESTART IDENTITY`，因此测试库必须与部署库隔离。

### 4. 准备 PostgreSQL

默认配置文件 [server/config/server.toml](server/config/server.toml) 使用以下数据库参数：

- `host=127.0.0.1`
- `port=5432`
- `name=nebula`
- `user=nebula`
- `password_env=NEBULA_DATABASE_PASSWORD`

服务需要一个可访问的 PostgreSQL 实例。本文默认示例使用 `docker.io/library/postgres:16` 容器镜像。

如果你希望直接按默认配置部署，可以先启动一个本地 PostgreSQL 容器：

以下命令中的 `nebula` 仅用于本地演示。生产环境请替换为强密码，并同步更新后续环境变量。

```bash
podman run -d \
    --name nebula-postgres \
    -e POSTGRES_DB=nebula \
    -e POSTGRES_USER=nebula \
    -e POSTGRES_PASSWORD=nebula \
    -p 5432:5432 \
    -v nebula-pgdata:/var/lib/postgresql/data \
    docker.io/library/postgres:16
```

等待数据库就绪：

```bash
until podman exec nebula-postgres pg_isready -U nebula -d nebula >/dev/null 2>&1; do
    sleep 1
done
```

### 5. 初始化数据库

首次部署需要执行表结构初始化脚本 [server/deploy/postgres/init_schema.sql](server/deploy/postgres/init_schema.sql)：

```bash
podman exec -i nebula-postgres \
    psql -U nebula -d nebula \
    < server/deploy/postgres/init_schema.sql
```

可选验证：

```bash
podman exec -it nebula-postgres psql -U nebula -d nebula -c '\dt'
```

### 6. 配置运行时环境

生产环境不要继续使用本文中的示例密码 `nebula`。正式部署前至少应完成以下替换：

- PostgreSQL 用户密码改为强密码
- `NEBULA_DATABASE_PASSWORD` 改为对应的新密码
- 数据库不要直接暴露在默认示例账号和口令下

启动服务前需要设置数据库密码环境变量：

```bash
export NEBULA_DATABASE_PASSWORD='<your-strong-password>'
```

如果你按本文的本地演示命令启动数据库，可以临时使用：

```bash
export NEBULA_DATABASE_PASSWORD=nebula
```

如果你使用自定义数据库实例，请同步修改 [server/config/server.toml](server/config/server.toml) 中的 `[database]` 配置，或准备一份新的配置文件并通过 `--config` 传入。

服务默认还会使用以下运行时路径：

- 日志目录：`runtime/logs`
- JWT 密钥文件：`runtime/secrets/jwt.key`

如果 `runtime/secrets/jwt.key` 不存在，服务会在首次启动时自动生成。文件中保存的是 Base64 编码后的随机密钥内容，服务启动时会先解码再用于 JWT 签名。生产环境应妥善持久化和保护该文件，避免因重新生成导致已签发令牌全部失效；如果手工提供该文件，请确保内容是合法的 Base64 编码密钥。

### 7. 启动服务

使用默认配置启动：

```bash
./server/build/bin/nebula
```

使用自定义配置文件启动：

```bash
./server/build/bin/nebula --config server/config/server.toml
```

默认监听端口为 `8080`。

### 8. 验证部署结果

健康检查：

```bash
curl --noproxy '*' -i http://127.0.0.1:8080/healthz
```

预期返回：

```http
HTTP/1.1 200 OK
Content-Type: application/json

{"status":"ok"}
```

根路径默认映射到 `/healthz`，因此也可以直接验证：

```bash
curl --noproxy '*' -i http://127.0.0.1:8080/
```

## License

[MIT License](LICENSE)
