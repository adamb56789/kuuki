set -eu

name="/kuuki/api_key"

aws ssm put-parameter --name $name --value $(openssl rand -hex 32) --overwrite
aws ssm get-parameter --name $name --query "Parameter.Value" --output text > env/api_key.txt