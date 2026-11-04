import {Construct} from "constructs";
import {Stack, StackProps} from "aws-cdk-lib";
import {Code, Function as LambdaFunction, FunctionUrlAuthType, Runtime} from "aws-cdk-lib/aws-lambda";
import * as path from "node:path";
import {TableV2} from "aws-cdk-lib/aws-dynamodb";
import {StringParameter} from "aws-cdk-lib/aws-ssm";

export class KuukiStack extends Stack {
    constructor(scope: Construct, id: string, props?: StackProps) {
        super(scope, id, props);

        const measurementsTableReference = TableV2.fromTableName(this, "MeasurementsTable", "MeasurementsTable");
        const apiKeyReference = StringParameter.fromStringParameterName(this, "apiKey", "/kuuki/api_key");

        const getLatestDataLambda = new LambdaFunction(this, "getLatestData", {
            functionName: "GetLatestData",
            runtime: Runtime.PROVIDED_AL2023,
            code: Code.fromAsset(path.join(__dirname, "..", "..", "cmake-build-release", "get_latest_data.zip")),
            handler: "handler", // unused by our runtime
            memorySize: 1769, // 1 CPU, can be decreased after testing
            reservedConcurrentExecutions: 1, // denial-of-wallet protection
            environment: {
                API_KEY: apiKeyReference.stringValue,
            },
        });

        measurementsTableReference.grantReadData(getLatestDataLambda);
        apiKeyReference.grantRead(getLatestDataLambda);

        getLatestDataLambda.addFunctionUrl({authType: FunctionUrlAuthType.NONE});
    }
}
