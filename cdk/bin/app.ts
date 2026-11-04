#!/usr/bin/env node
import { App } from 'aws-cdk-lib';
import { KuukiStack } from '../lib/kuuki_stack';

const app = new App();
new KuukiStack(app, 'KuukiStack', {
    env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: process.env.CDK_DEFAULT_REGION },
});
