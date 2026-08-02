/**
 * @file network_cli.h
 * @brief Authenticated TCP transport for the AD2IoT command line interface.
 */
#ifndef _NETWORK_CLI_H_
#define _NETWORK_CLI_H_

#if CONFIG_AD2IOT_NETWORK_CLI

void network_cli_register_cmds();
void network_cli_init();

#endif /* CONFIG_AD2IOT_NETWORK_CLI */
#endif /* _NETWORK_CLI_H_ */
