# Ril 说明文档

[[English](./README.md)|简体中文]


## **概述**

RIL（Radio Interface Layer）是一种用于 VELA 平台的架构组件，RIL 由 RILD、LIBRIL 和 REFERENCE-RIL 构成一个完整的无线通信架构，它作为 VELA 系统与基带之间的中介层，使操作系统能够与无线通信硬件进行交互。

![RIL架构图](./RIL.jpg)

## **项目目录**
```tree
├── include
│   └── telephony
│       ├── ril.h
│       └── ril_log.h
├── libril
│   ├── parcel.cpp
│   ├── parcel.h
│   ├── ril_commands.h
│   ├── ril.cpp
│   ├── ril_cus_commands.h
│   ├── ril_event.cpp
│   ├── ril_event.h
│   ├── ril_ims_commands.h
│   ├── ril_second_commands.h
│   └── ril_unsol_commands.h
├── reference-ril
│   ├── at_call.c
│   ├── at_call.h
│   ├── atchannel.c
│   ├── atchannel.h
│   ├── at_data.c
│   ├── at_data.h
│   ├── at_modem.c
│   ├── at_modem.h
│   ├── at_network.c
│   ├── at_network.h
│   ├── at_ril.c
│   ├── at_ril.h
│   ├── at_sim.c
│   ├── at_sim.h
│   ├── at_sms.c
│   ├── at_sms.h
│   ├── at_tok.c
│   └── at_tok.h
├── rild
│   └── rild.c
├── README.md
└── README_zh-cn.md
```

## **模块介绍**

| 模块     | 文件  | 说明      |
| :------ | :------- | :--------- |
| Rild | rild.c  | <div style="width: 150pt">初始化 RIL 层 |
| LibRil | libril/* | AT 命令的转换与数据的解析 |
| Reference-ril | reference-ril/* | 向 Modem 发送与接收指令 |

### **功能介绍**

#### Rild
- 初始化 RIL 层：初始化 LibRil 以及启动 Reference RIL。
- 接收上层请求：将应用程序的通信请求发给 LibRil 处理

#### LibRil
- 请求转换和命令发送‌：负责将上层应用发出的请求转换为 AT 命令，并传递给 Reference-ril 层。
- 请求接收和命令响应：Reference-ril 响应的数据会通过 LibRil 进行解析，确保上层应用能够正确处理这些响应‌。

#### VendorRil
- 主动请求命令：向 Modem 发送 AT 指令。
- 接收指令：接收来自 LibRil 或者 Modem 的指令。

