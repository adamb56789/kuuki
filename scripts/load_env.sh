set -ex

creds_file=$(find ~/.aws/login/cache/ -name *json -print -quit)

five_minutes_from_now=$(( $(date -u +%s) + 300 ))

if [[ -z $creds_file ]]; then
    expiry_time=0
else
    iso_expiry=$(jq -r ".accessToken.expiresAt" $creds_file 2>/dev/null)
    expiry_time=$(date -u -d "$iso_expiry" +%s 2>/dev/null || echo 0)
fi

if [[ $expiry_time -lt $five_minutes_from_now ]]; then
  unset AWS_ACCESS_KEY_ID # Stop it from trying to use the env vars
  aws sts get-caller-identity > /dev/null || exit 1
fi

export AWS_ACCESS_KEY_ID=$(jq -r ".accessToken.accessKeyId" $creds_file )
export AWS_SECRET_ACCESS_KEY=$(jq -r ".accessToken.secretAccessKey" $creds_file )
export AWS_SESSION_TOKEN=$(jq -r ".accessToken.sessionToken" $creds_file )

if [ -f env/api_key.txt ]; then
    export API_KEY=$(<env/api_key.txt)
fi
