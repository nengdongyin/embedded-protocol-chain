cmake_minimum_required(VERSION 3.10.0)
project(embedded-protocol-chain VERSION 0.1.0 LANGUAGES C)

add_executable(embedded-protocol-chain
 # app
 protocol_parser/app/protocol_parser_test_windows.c
 # core
 protocol_parser/core/protocol_parser.c
 protocol_parser/core/protocol_parser.h
 protocol_parser/core/protocol_parser_internal.h
 protocol_parser/core/protocol_chain.c
 protocol_parser/core/protocol_chain.h
 protocol_parser/core/system_adapter.h
 # protocols/camyu
 protocol_parser/protocols/camyu/protocol_parser_camyu.c
 protocol_parser/protocols/camyu/protocol_parser_camyu.h
 # protocols/imperx
 protocol_parser/protocols/imperx/protocol_parser_imperx.c
 protocol_parser/protocols/imperx/protocol_parser_imperx.h
 # protocols/ymodem
 protocol_parser/protocols/ymodem/adapter/protocol_parser_ymodem.c
 protocol_parser/protocols/ymodem/adapter/protocol_parser_ymodem.h
 protocol_parser/protocols/ymodem/src/ymodem_common.c
 protocol_parser/protocols/ymodem/src/ymodem_common.h
 protocol_parser/protocols/ymodem/src/ymodem_receiver.c
 protocol_parser/protocols/ymodem/src/ymodem_receiver.h
 protocol_parser/protocols/ymodem/src/ymodem_sender.c
 protocol_parser/protocols/ymodem/src/ymodem_sender.h
)
