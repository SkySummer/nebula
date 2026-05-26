# Nebula

Nebula 是一个基于 C++23 的服务端项目，使用 PostgreSQL 作为持久化存储，采用 Conan 2 + CMake 构建。主要依赖包括 gRPC、libpqxx、OpenSSL。

## 部署流程

以下步骤适用于 Linux 环境，命令默认在仓库根目录执行。容器命令以 `podman` 为例，使用 `docker` 时将命令中的 `podman` 替换为 `docker` 即可。

### 1. 环境准备

需要以下工具：

- GCC 13+ 或其他兼容 C++23 的编译器
- CMake 3.20+
- Conan 2.x
- Podman 或 Docker

首次使用 Conan 2 时，先生成本机 profile：

```bash
conan profile detect
```

### 2. 构建服务

构建 Release 版本：

```bash
conan install server \
    --output-folder=server/build/release \
    --build=missing \
    -pr=server/conanprofile \
    -s build_type=Release

cmake --preset release
cmake --build --preset release --parallel
```

构建产物位于 `./server/build/release/bin/nebula`。

### 3. 准备 PostgreSQL

默认配置文件 [server/config/server.toml](server/config/server.toml) 中的数据库参数：

```toml
[database]
host=127.0.0.1
port=5432
name=nebula
user=nebula
password_env=NEBULA_DATABASE_PASSWORD
```

服务需要一个可访问的 PostgreSQL 实例。如果直接按默认配置部署，可以启动一个本地容器：

> 以下命令中的密码 `nebula` 仅用于本地演示。生产环境请替换为强密码，并同步更新 `NEBULA_DATABASE_PASSWORD`。

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

确认数据库就绪（输出包含 `accepting connections` 即可继续）：

```bash
podman exec nebula-postgres pg_isready -U nebula -d nebula
```

如果使用自定义数据库实例，请同步修改 [server/config/server.toml](server/config/server.toml) 中的 `[database]` 段，或准备一份新的配置文件并通过 `--config` 传入。

### 4. 初始化数据库

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

### 5. 配置运行时

启动服务前需要设置数据库密码：

```bash
export NEBULA_DATABASE_PASSWORD='<your-strong-password>'
```

如果使用本文示例的本地演示数据库，可临时设为：

```bash
export NEBULA_DATABASE_PASSWORD=nebula
```

服务运行时使用以下默认路径：

- 日志目录：`runtime/logs`
- JWT 密钥文件：`runtime/secrets/jwt.key`
- 文件存储目录：`runtime/files`

关于 JWT 密钥文件：

- 如果 `runtime/secrets/jwt.key` 不存在，服务在首次启动时会自动生成，文件内容为 Base64 编码后的随机密钥，服务启动时先解码再用于 JWT 签名。
- 生产环境应妥善持久化和保护该文件。重新生成会导致已签发的令牌全部失效。
- 如果手工提供该文件，请确保内容是合法的 Base64 编码密钥。

### 6. 启动服务

使用默认配置启动：

```bash
./server/build/release/bin/nebula
```

显式指定配置文件：

```bash
./server/build/release/bin/nebula --config server/config/server.toml
```

服务默认监听 `8080` 端口。

### 7. 验证部署

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

## 运行测试

如果希望在部署前完整验证，构建 Debug 版本并执行测试：

```bash
conan install server \
    --output-folder=server/build/debug \
    --build=missing \
    -pr=server/conanprofile \
    -s build_type=Debug

cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

说明：

- `ctest --preset debug` 会执行全部已注册测试。
- 数据库相关测试依赖 `NEBULA_TEST_DATABASE_*` 环境变量，未配置时这些用例会直接失败。如需完整执行，请按下文准备独立的测试数据库。

### 准备测试数据库

> 测试用例会执行 `TRUNCATE TABLE users RESTART IDENTITY`，因此**必须使用独立的测试数据库，不要复用生产数据库**。

启动测试库（端口 `15432`，与生产实例隔离）：

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

确认数据库就绪：

```bash
podman exec nebula-postgres-test pg_isready -U nebula -d nebula_test
```

初始化测试库表结构：

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

重新执行：

```bash
ctest --preset debug
```

## 生产部署清单

正式部署前至少完成以下事项：

- 将 PostgreSQL 用户密码替换为强密码，并同步更新 `NEBULA_DATABASE_PASSWORD`
- 不要在公网环境使用默认账号 `nebula`
- 妥善持久化并备份 `runtime/secrets/jwt.key`，避免令牌失效
- 关闭或限制测试库（端口 `15432`）对外暴露

## License

[MIT License](LICENSE)
