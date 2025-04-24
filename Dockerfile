FROM node:20

WORKDIR /app
COPY . .

# For native fetch and top-level await support
RUN apt update && apt install -y curl && npm install

CMD ["node", "patch.mjs"]
