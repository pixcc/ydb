# Deploying {{ ydb-short-name }} with Federated Query functionality

{% note warning %}

This functionality is in the "Experimental" mode.

{% endnote %}

## General Installation Scheme {#general-scheme}

{{ ydb-full-name }} can perform [federated queries](../../../../concepts/federated_query/index.md) to external sources, for example, object storages or relational DBMS, without the need to move the data from external sources directly into {{ ydb-short-name }}. This section describes the changes that are required in the configuration of {{ ydb-short-name }} and the surrounding infrastructure to enable federated queries.

{% note info %}

A special microservice called [connector](../../../../concepts/federated_query/architecture.md#connectors) must be deployed to access some of data sources. Check the [list of supported sources](../../../../concepts/federated_query/architecture.md#supported-datasources) to determine if you need to install a connector.

{% endnote %}

The {{ ydb-short-name }} cluster and external data sources in a production installation should be deployed on different physical or virtual servers, including clouds. If access to a specific source requires a connector, it should be deployed on the same servers as the dynamic nodes of {{ ydb-short-name }}. In other words, each `ydbd` process running in dynamic node mode should have one local connector process.

The following requirements must be met:

* The external data source must be accessible over the network to queries from {{ ydb-short-name }} database nodes or from the connector, if present.
* The connector must be accessible over the network from {{ ydb-short-name }} database nodes.

 {% note tip %}

 The easiest way to make the connector accessible from {{ ydb-short-name }} nodes is to run them on the same set of hosts.

 {% endnote %}

![{{ ydb-short-name }} FQ Installation](_images/ydb_fq_onprem.png "{{ ydb-short-name }} FQ Installation" =1024x)

{% note info %}

Currently, we do not support deploying the connector in {{ k8s }}, but we plan to add it shortly.

{% endnote %}

## Step-by-Step Guide

1. Follow the steps in the dynamic node {{ ydb-short-name }} deployment guide up to and including [preparing the configuration files](../initial-deployment.md#config).
2. If a connector must be deployed to access the desired source, do so [according to the instructions](./connector-deployment.md).
3. If a connector needs to be deployed to access your desired source, add the `generic` subsection to the `query_service_config` section of the {{ ydb-short-name }} configuration file as shown below. Specify the network address of the connector in the `connector.endpoint.host` and `connector.endpoint.port` fields (default values are `localhost` and `2130`). When co-locating the connector and the {{ ydb-short-name }} dynamic node on the same server, encrypted connections between them are *not required*. If necessary, you can enable encryption by setting `connector.use_ssl` to `true` and specifying the path to the CA certificate that is used to sign the connector's TLS keys in `connector.ssl_ca_crt`:

    ```yaml
    query_service_config:
        generic:
            connector:
                endpoint:
                    host: localhost                 # hostname where the connector is deployed
                    port: 2130                      # port number for the connector's listening socket
                use_ssl: false                      # flag to enable encrypted connections
                ssl_ca_crt: "/opt/ydb/certs/ca.crt" # (optional) path to the CA certificate
            default_settings:
                - name: DateTimeFormat
                  value: string
                - name: UsePredicatePushdown
                  value: "true"
    ```

4. Add the following `feature_flags` section to the {{ ydb-short-name }} configuration file:

    ```yaml
    feature_flags:
        enable_external_data_sources: true
        enable_script_execution_operations: true
    ```

5. Continue deploying {{ ydb-short-name }} database nodes. See the [instructions](../initial-deployment.md).
